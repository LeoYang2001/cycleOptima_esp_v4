# Quick Fix Summary: Batch Timer Delay Bug

## The Problem

When loading batch 3+ during cycle execution:
- Batch timer delays set to 189+ seconds (should be ~5 seconds)
- GPIO stays OFF (all 1s) instead of changing  
- Events fire but GPIO doesn't reflect them

**Console showed:**
```
I (346327) cycle: Batch timer set for 189.401 seconds  ← WRONG!
[195834 ms] GPIO: 7:1 8:1 5:1 19:1 9:1 18:1 4:1 10:1  ← All OFF!
```

## The Root Cause

In `load_next_batch_timer_cb()`, the batch timer delay was calculated as:
```c
uint64_t last_event_time_us = 346000000;  // Absolute time (346 seconds)
uint64_t delay_to_next_batch = last_event_time_us + 1000;  // Treated as delay!
esp_timer_start_once(timer, delay_to_next_batch);  // Waits 346 seconds!
```

The bug: Treating an **absolute event time** as a **relative delay**.

## The Fix

Calculate delay by subtracting elapsed time from absolute event time:

```c
uint64_t last_event_time_us = 346000000;  // Absolute time (346 seconds)
uint64_t time_since_phase_start = 346000000;  // Time elapsed
uint64_t delay_to_next_batch = (last_event_time_us - time_since_phase_start) + 1000;
// = (346 - 346) + 1 = 1 millisecond ✓

esp_timer_start_once(timer, delay_to_next_batch);  // Waits 1ms correctly!
```

## Changes Made

**File:** `main/cycle.c`  
**Function:** `load_next_batch_timer_cb()`  
**Lines:** 498-551

### Before:
```c
// Event timer delay (unclear variable name)
uint64_t elapsed = now_us - batch_start_us;
...
// Batch timer delay (WRONG calculation)
uint64_t delay_to_next_batch = last_event_time_us + 1000;
```

### After:
```c
// Event timer delay (clearer variable name, added comment)
uint64_t time_since_phase_start = now_us - batch_start_us;
...
// Batch timer delay (CORRECT calculation with safety check)
uint64_t delay_to_next_batch;
if (last_event_time_us > time_since_phase_start) {
    delay_to_next_batch = (last_event_time_us - time_since_phase_start) + 1000;
} else {
    delay_to_next_batch = 1000;
}
```

## Expected Result

**After Fix - Console Output:**
```
I (346317) cycle: Loading batch 3/8 (200 events)
I (346327) cycle: Batch timer set for 5.123 seconds  ← CORRECT! ~5 seconds
[346350 ms] GPIO: 7:0 8:1 5:0 19:1 9:1 18:1 4:1 10:1  ← GPIO CHANGES! ✓
[346400 ms] GPIO: 7:1 8:0 5:1 19:1 9:1 18:0 4:0 10:1  ← More changes! ✓
```

## How to Deploy

```bash
# Build
idf.py build

# Flash
idf.py flash

# Monitor (watch for correct batch timer delays ~5-10s)
idf.py monitor
```

## What to Verify

- [ ] Build: `idf.py build` shows zero errors
- [ ] Flash: Device flashes successfully
- [ ] Test: Load 1600-event cycle
- [ ] Console: Batch timers show ~5-10 seconds (not 189 seconds)
- [ ] GPIO: Changes visible during all batches (not just batch 1)
- [ ] Phase completes: "Phase finished" message appears

## Status

✅ **READY** - Code compiles, fix applied, documentation complete
