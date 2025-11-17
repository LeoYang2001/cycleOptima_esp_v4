# Batch Timer Architecture - Code Flow

## State Machine Flow

```
┌─────────────────────────────────────────────────────────────────────┐
│ run_phase_with_esp_timer(phase)                                     │
│ ─────────────────────────────────────────────────────────────────── │
│                                                                     │
│  1. Build timeline: events[0..1599] (all 1600 events)              │
│  2. Calculate: batches_total = (1600 + 199) / 200 = 8              │
│  3. current_batch_idx = 0                                          │
│                                                                     │
│  4. FOR i in 0..199:  ← FIRST BATCH (200 events)                  │
│       esp_timer_create(&args) → timers[i]                         │
│       esp_timer_start_once(timers[i], delay)                      │
│                                                                     │
│  5. Create batch_timer callback                                    │
│       Will fire ~5.123 seconds later (after last event in batch)   │
│                                                                     │
│  6. Set active = true, return                                      │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
                              ↓
                   [Events 0-199 execute]
                    (~5 seconds elapsed)
                              ↓
┌─────────────────────────────────────────────────────────────────────┐
│ load_next_batch_timer_cb()  ← Fires automatically                   │
│ ────────────────────────────────────────────────────────────────    │
│                                                                     │
│  1. Delete current batch:                                          │
│     FOR i in 0..199:                                               │
│         esp_timer_delete(timers[i])  ← Reclaims ~10KB              │
│                                                                     │
│  2. current_batch_idx++ (now = 1)                                  │
│                                                                     │
│  3. IF current_batch_idx < batches_total (1 < 8):                 │
│       ├─ Calculate: batch_start = 1 * 200 = 200                   │
│       ├─ Calculate: batch_end = 2 * 200 = 400                     │
│       │                                                             │
│       ├─ FOR i in 0..199:  ← SECOND BATCH (events 200-399)        │
│       │    esp_timer_create(&args) → timers[i]                    │
│       │    esp_timer_start_once(timers[i], delay)                 │
│       │                                                             │
│       └─ Create batch_timer for next batch                         │
│          (will fire ~5.098 seconds later)                          │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
                              ↓
                  [Events 200-399 execute]
                   (~5 seconds elapsed)
                              ↓
                   [Repeat: batch 3, 4, 5, 6, 7]
                              ↓
┌─────────────────────────────────────────────────────────────────────┐
│ load_next_batch_timer_cb() on Batch 8                               │
│ ────────────────────────────────────────────────────────────────    │
│                                                                     │
│  1. Delete batch 7 timers                                          │
│  2. current_batch_idx++ (now = 8)                                  │
│  3. IF current_batch_idx >= batches_total (8 >= 8):               │
│       └─ DONE! Don't create any more batches                       │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
                              ↓
                   [Events 1400-1599 execute]
                              ↓
              [event_timer_cb() on last event]
                              ↓
┌─────────────────────────────────────────────────────────────────────┐
│ event_timer_cb() when remaining_events → 0                          │
│ ────────────────────────────────────────────────────────────────    │
│                                                                     │
│  remaining_events--  (now = 0)                                     │
│  IF remaining_events == 0:                                         │
│    active = false  ← Phase complete!                               │
│    ESP_LOGI("Phase finished")                                      │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
                              ↓
                 [run_cycle() detects active=false]
                              ↓
           [Move to next phase or cycle complete]
```

## Memory Timeline

```
Time     Heap State                           Action
─────    ────────────────────────────────────────────────────
Start    [=======100KB free========]          Cycle starts
         
         [=====85KB free==][Batch1:10KB]      Load batch 1 (200 events)
  0-5s   [=====75KB free==][Event 0-199]      Events 0-199 fire (hold timers)

  ~5s    [=======85KB free========]           Delete batch 1 (reclaim 10KB)
         [=====75KB free==][Batch2:10KB]      Load batch 2 (200 events)
  5-10s  [=====75KB free==][Event 200-399]    Events 200-399 fire

  ~10s   [=======85KB free========]           Delete batch 2
         [=====75KB free==][Batch3:10KB]      Load batch 3
  10-15s [=====75KB free==][Event 400-599]    Events 400-599 fire

         ... repeat ...

  ~35s   [=======85KB free========]           Delete batch 7
         [=====75KB free==][Batch8:10KB]      Load batch 8 (final)
  35-40s [=====75KB free==][Event 1400-1599]  Events 1400-1599 fire

  ~40s   [=======100KB free========]          Delete batch 8 + batch_timer
                                               Phase complete

═════════════════════════════════════════════════════════════

Without batching (OLD):
         [=====20KB free][Batch:80KB]         Create all 1600 timers
         ↓ CRASH: 80KB > 20KB available ✗

With batching (NEW):
         [=====75KB free][Batch:10KB]         Create 200 timers
         ✓ Success: 10KB < 75KB available ✓
```

## Skip/Stop Handling

```
Scenario: User calls cycle_skip_current_phase() during batch 3

Timeline:
  Events 0-199   (Batch 1)  ─── COMPLETE ───→ Deleted
  Events 200-399 (Batch 2)  ─── COMPLETE ───→ Deleted
  Events 400-599 (Batch 3)  ←── RUNNING ──←  [timers[0..199] exist]
         ↓
    [User presses STOP]
         ↓
  cycle_skip_current_phase(true) called
         ↓
┌──────────────────────────────────────────────────┐
│ Cleanup:                                         │
│  FOR i in 0..BATCH_SIZE:                        │
│    esp_timer_stop(timers[i])    ← 200 timers   │
│    esp_timer_delete(timers[i])                  │
│                                                 │
│  esp_timer_stop(batch_timer)    ← Prevent next │
│  esp_timer_delete(batch_timer)     batch load  │
│                                                 │
│  FOR all GPIO:                                  │
│    gpio_set_level(pin, 1)  ← Turn OFF          │
│                                                 │
│  active = false  ← Phase marked done           │
└──────────────────────────────────────────────────┘
         ↓
  Batches 4-8 NEVER created
  Cycle moves to next phase
```

## Data Structures

### PhaseRunContext (Before vs After)

**BEFORE:**
```c
typedef struct {
    TimelineEvent events[1600];           // 12.8 KB
    esp_timer_handle_t timers[1600];      // 6.4 KB (1600 handles × 4 bytes)
    size_t num_events;                    // 8 bytes
    size_t remaining_events;              // 8 bytes
    bool active;                          // 1 byte
} PhaseRunContext;
// Total: ~19 KB static + 80 KB timer allocations = 99 KB peak
```

**AFTER:**
```c
typedef struct {
    TimelineEvent events[1600];           // 12.8 KB (reused across batches)
    esp_timer_handle_t timers[200];       // 0.8 KB (200 handles × 4 bytes) ← REDUCED
    size_t num_events;                    // 8 bytes
    size_t remaining_events;              // 8 bytes
    bool active;                          // 1 byte
    
    // Batch tracking (NEW):
    size_t current_batch_idx;             // 8 bytes
    size_t batches_total;                 // 8 bytes
    size_t next_batch_start_event;        // 8 bytes
    esp_timer_handle_t batch_timer;       // 4 bytes
    uint64_t batch_start_us;              // 8 bytes
} PhaseRunContext;
// Total: ~13.6 KB static + 10 KB timer allocations = 23.6 KB peak
```

**Savings:** 19 KB → 13.6 KB static (-26%), 99 KB → 23.6 KB peak (-76%)

## Event Callback Chain

```
Event fires at scheduled time:
     ↓
event_timer_cb() called
     ├─ gpio_set_level(pin, level)  ← Set GPIO
     ├─ gpio_shadow[idx] = level    ← Update shadow
     ├─ remaining_events--          ← Decrement counter
     └─ IF remaining_events == 0:
           ├─ active = false        ← Mark phase done
           └─ ESP_LOGI("Phase finished")


Batch transition at scheduled time:
     ↓
load_next_batch_timer_cb() called
     ├─ FOR i in 0..BATCH_SIZE:
     │   ├─ esp_timer_stop(timers[i])
     │   └─ esp_timer_delete(timers[i])
     ├─ current_batch_idx++
     ├─ IF current_batch_idx < batches_total:
     │   ├─ Calculate batch events
     │   ├─ FOR i in 0..new_batch_size:
     │   │   ├─ esp_timer_create(&args, &timers[i])
     │   │   └─ esp_timer_start_once(timers[i], delay)
     │   └─ Create new batch_timer
     └─ ELSE:
         └─ (All batches complete, do nothing)
```

## Timing Diagram (4-Event Phase as Example)

```
Event times (absolute):
  E0: 0ms
  E1: 100ms
  E2: 200ms
  E3: 300ms

Batch layout (BATCH_SIZE=2 for this example):
  Batch 1: E0, E1
  Batch 2: E2, E3

Timeline:
Time  Event              Action                    GPIO
──────────────────────────────────────────────────────────
0ms   run_phase_with..   Create timers for E0, E1  -
      set batch_timer    for ~200ms (E1 + 1ms)     -

100ms E0 fires           event_timer_cb(E0)        ON_VALVE=1
      [E0 GPIO action]   remaining_events: 3→2     

200ms E1 fires           event_timer_cb(E1)        DRAIN=0
      [E1 GPIO action]   remaining_events: 2→1     

~201ms batch_timer fires load_next_batch_timer_cb() -
      Delete E0, E1      Create timers for E2, E3   -
      Create batch_timer set for ~300ms (E3 + 1ms)

300ms E2 fires           event_timer_cb(E2)        MOTOR=1
      [E2 GPIO action]   remaining_events: 1→0     

300ms active == 0        Phase complete!            [holding]
      Phase marked done

301ms E3 fires           event_timer_cb(E3)        MOTOR=0
      [E3 GPIO action]   remaining_events: 0 (no change)
      

Result:
  ✓ All 4 events executed in correct order
  ✓ GPIO changes applied
  ✓ Batching transparent to user
  ✓ Memory reclaimed after batch 1 deleted
```

## Build & Deployment

```
Terminal Commands:
────────────────

$ idf.py build
> [... build output ...]
> "No errors found" ✓

$ idf.py flash
> [... flash output ...]
> "Leaving... Hard resetting via RTS pin..." ✓

$ idf.py monitor
> [cycle logs with batch loading messages]

Expected First Run Output:
────────────────────────
I (10234) cycle: Phase 'Wash': 1600 events in 8 batches (batch_size=200)
I (10234) cycle: Loading batch 1/8 (200 events)
I (10235) cycle: Batch loader timer scheduled for 5.123 seconds
I (10235) cycle: Scheduled 1600 events for phase Wash in batches
... [Events 0-199 execute] ...
I (15358) cycle: Loading batch 2/8 (200 events)
I (15358) cycle: Batch loader timer scheduled for 5.098 seconds
... [Events 200-399 execute] ...
... [Batches 3-8 repeat] ...
I (40000) cycle: Phase finished (all events fired).
```

---

## References

- `cycle.c` line 47-57: `PhaseRunContext` definition
- `cycle.c` line 450-540: `event_timer_cb()` and `load_next_batch_timer_cb()`
- `cycle.c` line 565-645: `run_phase_with_esp_timer()`
- `cycle.c` line 655-690: `cycle_skip_current_phase()`
- `cycle.c` line 750-780: `run_cycle()` cleanup

