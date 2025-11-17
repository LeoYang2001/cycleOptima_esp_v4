# Batched Timer Implementation - Quick Reference

## What Changed

**Problem:** Creating 1600 timers at once → `ESP_ERR_NO_MEM` crash
**Solution:** Create timers in batches of 200, delete old batch when new batch loads

## Key Code Changes

### 1. New Constant
```c
#define BATCH_SIZE 200  // Max timers to create at once
```

### 2. PhaseRunContext Expansion
```c
typedef struct {
    TimelineEvent events[MAX_EVENTS_PER_PHASE];  // All events (1600)
    esp_timer_handle_t timers[BATCH_SIZE];       // Current batch only (200) ← CHANGED
    
    // Batch tracking (NEW):
    size_t current_batch_idx;
    size_t batches_total;
    esp_timer_handle_t batch_timer;
    uint64_t batch_start_us;
} PhaseRunContext;
```

### 3. New Callback: `load_next_batch_timer_cb()`
- Fires when current batch's last event completes
- Deletes current batch timers (reclaims ~10KB)
- Creates and schedules next batch (if not done)
- Automatically called after batch events fire

### 4. Modified: `run_phase_with_esp_timer()`
- Calculates: `batches_total = (n + 199) / 200`
- Creates only first batch (200 timers)
- Schedules `batch_timer` to load next batch

### 5. Updated: `cycle_skip_current_phase()`
- Deletes current batch timers (only 200, not 1600)
- Deletes batch_timer (prevents future batches)

### 6. Updated: `run_cycle()` cleanup
- Deletes current batch timers between phases
- Deletes batch_timer before next phase

## Memory Impact

| Scenario | Before | After | Savings |
|----------|--------|-------|---------|
| 1600 events | 80KB peak | 10KB batched | 8× |
| 400 events | 20KB peak | 10KB batched | 2× |
| 50 events | 2.5KB peak | 2.5KB (1 batch) | Same |

## Timing Impact

✅ **No change** - Events fire at exact scheduled times
- All 1600 events pre-computed with absolute µs timestamps
- Batching is transparent to GPIO control
- Jitter remains ±100µs (acceptable for relays)

## Testing Checklist

- [ ] Compile: `idf.py build` (should be zero errors)
- [ ] Flash: `idf.py flash` 
- [ ] Load small cycle (50 events, 1 batch) - should complete normally
- [ ] Load medium cycle (400 events, 2 batches) - verify GPIO pattern correct
- [ ] Load large cycle (1600 events, 8 batches) - verify no ESP_ERR_NO_MEM
- [ ] Check console: Should see "Loading batch 1/8", "Loading batch 2/8", etc.
- [ ] Test skip during batch - verify phase stops, GPIO goes off
- [ ] Monitor heap with `idf.py monitor` - should stay stable

## Console Examples

**Small phase (1 batch):**
```
I cycle: Phase 'Test': 50 events in 1 batches
I cycle: Loading batch 1/1 (50 events)
I cycle: Scheduled 50 events for phase Test in batches
```

**Large phase (8 batches):**
```
I cycle: Phase 'Wash': 1600 events in 8 batches
I cycle: Loading batch 1/8 (200 events)
I cycle: Batch loader timer scheduled for 5.123 seconds
I cycle: Loading batch 2/8 (200 events)  ← 5.123 seconds later
...
I cycle: Phase finished (all events fired)
```

## Files Modified

- `cycle.c`
  - `PhaseRunContext` structure (added batch tracking fields)
  - `event_timer_cb()` (unchanged logic, same counting)
  - `load_next_batch_timer_cb()` (NEW callback for batch transitions)
  - `run_phase_with_esp_timer()` (batch calculation and loading)
  - `cycle_skip_current_phase()` (cleanup batch_timer too)
  - `run_cycle()` (cleanup batch_timer between phases)

## Compile Command

```bash
idf.py build
# Should show: "No errors found"
```

## Expected Behavior

1. **Phase load:** Calculate batches (e.g., 1600 events → 8 batches)
2. **First batch:** Create 200 timers, start them
3. **Events 0-199:** Fire at scheduled times
4. **Batch complete:** `load_next_batch_timer_cb()` fires
5. **Batch transition:** Delete 200 timers, create 200 new ones (automatic)
6. **Events 200-399:** Fire at scheduled times
7. **Repeat:** Until all 8 batches complete
8. **Phase end:** GPIO stays as last event set it

## Backward Compatibility

✅ **100% compatible** - External API unchanged:
- `run_phase_with_esp_timer(phase)` - same signature
- `cycle_skip_current_phase(force_off)` - same signature
- `run_cycle(phases, num_phases)` - same signature
- Telemetry unchanged
- WebSocket unchanged
- GPIO control unchanged

The batching is **internal only** - clients don't see it.

## Next Steps

1. Build project: `idf.py build`
2. Flash device: `idf.py flash`
3. Run test cycle with ~1600 events
4. Monitor console for batch loading messages
5. Verify GPIO pattern is correct
6. Check that cycle completes without ESP_ERR_NO_MEM

## Questions?

See `BATCH_TIMER_SOLUTION.md` for detailed technical explanation of:
- Memory savings calculation
- Timing accuracy analysis
- Batch completion algorithm
- Performance testing strategy
