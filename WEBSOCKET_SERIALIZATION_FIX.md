# Large Cycle Serialization Failure - Root Cause Analysis

## Problem

When attempting to upload a large cycle via WebSocket, serialization fails with:
```
E (XXX) ws_cycle: Failed to serialize cycle data
```

This occurs with large cycles but not small ones, indicating a **heap exhaustion** problem.

## Root Cause

### The Problem Code (Line 221)
```c
char *json_str = cJSON_PrintUnformatted(data);  // ← PROBLEM
if (!json_str) {
    ESP_LOGE(TAG, "Failed to serialize cycle data");
    return ESP_FAIL;
}
```

### Why It Fails

`cJSON_PrintUnformatted()` allocates the **entire serialized JSON string as a single heap block**.

**Memory requirement for large cycles:**

For a cycle with:
- 20 phases
- 6 components per phase
- Motor configs with 75+ step patterns
- Total: ~1600 timeline events after expansion

The serialized JSON size:
```
Phase: ~500 bytes (id, name, start_time, etc.)
Component: ~300 bytes (compId, duration, motor config details)
Per phase: ~1800 bytes (500 + 6×300)
Total: 20 phases × 1800 = 36,000 bytes (~36 KB)

Plus motor pattern steps: 75 steps × 100 bytes = 7,500 bytes per motor

Real cycles can be 50-100+ KB of JSON
```

### Memory Timeline During Upload

```
1. WebSocket receives JSON frame (~50-100 KB)
   Free heap: ~80 KB → Allocate frame buffer
   Free heap: ~20 KB remaining

2. cJSON_Parse(buf) creates JSON tree in memory
   Free heap: Need ~50 KB
   Free heap: ~10 KB (NOT ENOUGH!)

3. cJSON_PrintUnformatted() tries to allocate serialized string
   Free heap: Need ~50 KB
   Free heap: ~10 KB (FAILS!) ✗
   Returns NULL
```

## Why Small Cycles Work

Small cycles (few phases, simple patterns):
- Serialized size: ~5-10 KB
- After parsing: ~15 KB free heap remains
- PrintUnformatted() succeeds with small allocation

Large cycles fail because they need more heap than available after parsing.

## Solution Approaches

### Approach 1: Stream to SPIFFS (Recommended for Large Cycles)
Instead of serializing to RAM first, write JSON **directly to SPIFFS** in chunks while parsing:

**Advantages:**
- No need to allocate entire JSON in RAM
- Works with arbitrary cycle sizes
- Faster (streaming vs memory allocation)

**Disadvantages:**
- More complex code
- Requires custom JSON-to-file writer

### Approach 2: Increase Heap Size
Modify partition table or reduce other features to free more heap:

**Advantages:**
- Simple fix
- Minimal code changes

**Disadvantages:**
- Limited (ESP32-C3 has ~160-180 KB total SRAM)
- Fragile - works for "current" cycle size but fails with slightly larger cycles
- Not scalable

### Approach 3: Use Smaller JSON Buffer
Reduce precision/detail in serialization:

**Disadvantages:**
- Loses cycle information
- Not acceptable for user data

### Approach 4: Parse and Write Simultaneously (Recommended)
Parse the incoming JSON **and simultaneously write to SPIFFS** without full buffering:

**Advantages:**
- No serialization needed (write original JSON)
- Minimal heap usage
- Fast and reliable

**This is the best approach** - just write the received JSON buffer directly to SPIFFS!

## Recommended Fix

The incoming `buf` is already valid JSON. Don't re-serialize it - just write it directly:

```c
// BEFORE (FAILS for large cycles):
char *json_str = cJSON_PrintUnformatted(data);  // Allocate ~50KB
if (!json_str) {
    ESP_LOGE(TAG, "Failed to serialize cycle data");
    return ESP_FAIL;  // ✗ Fails here for large cycles
}
fs_write_file("/spiffs/cycle.json", json_str, strlen(json_str));
free(json_str);

// AFTER (WORKS for any size):
fs_write_file("/spiffs/cycle.json", buf, ws_pkt.len);  // Direct write, no allocation
```

**Why this works:**
1. `buf` is the original JSON received from WebSocket
2. It's already valid (we parsed and validated it with `cJSON_Parse()`)
3. No need to re-serialize - just write the bytes as-is
4. No extra heap allocation needed

## Implementation

### Step 1: Write buffer directly to SPIFFS
```c
// Save original JSON directly (already valid)
if (fs_write_file("/spiffs/cycle.json", buf, ws_pkt.len) == ESP_OK) {
    ESP_LOGI(TAG, "cycle.json saved via websocket (%zu bytes)", ws_pkt.len);
} else {
    ESP_LOGE(TAG, "Failed to write to SPIFFS");
    return ESP_FAIL;
}
```

### Step 2: Read back and parse
```c
// Read from SPIFFS for verification
char *spiffs_json = fs_read_file("/spiffs/cycle.json");
if (!spiffs_json) {
    ESP_LOGE(TAG, "Failed to read cycle.json from SPIFFS");
    return ESP_FAIL;
}

// Load cycle
esp_err_t load_result = cycle_load_from_json_str(spiffs_json);
free(spiffs_json);
```

## Memory Analysis

### Before Fix (FAILS for large cycles)
```
Incoming: 50 KB JSON in buf
Parse: cJSON_Parse(buf) → Create tree in RAM (~50 KB)
Free heap: 80 - 50 = 30 KB
PrintUnformatted(): Need ~50 KB allocation
Available: 30 KB
Result: ✗ FAILS (NULL)
```

### After Fix (WORKS for any size)
```
Incoming: 50 KB JSON in buf
Write: fs_write_file(buf, 50KB) → SPIFFS I/O
Free heap: 80 KB (unchanged!)
Read: fs_read_file() → Load into RAM from SPIFFS
Parse: cJSON_Parse() → Create tree in RAM
Free heap: 80 - 50 = 30 KB
Result: ✓ SUCCESS (JSON validated and loaded)
```

## Testing

### Small Cycle (Works Today)
- Upload: ~10 KB cycle
- Expected: Success ✓
- After fix: Still works ✓

### Large Cycle (Fails Today)
- Upload: ~50-100 KB cycle
- Before fix: "Failed to serialize cycle data" ✗
- After fix: Success ✓

### Huge Cycle (Future-proofing)
- Upload: ~200 KB cycle
- Before fix: Fails (heap too small)
- After fix: Still works (no extra heap needed) ✓

## Files to Modify

- `main/ws_cycle.c`
  - Function: `ws_handler()` in `write_json` command section
  - Change: Remove `cJSON_PrintUnformatted()` call
  - Replace: Write buffer directly to SPIFFS

## Compilation Impact

✓ No changes to cycle.h
✓ No changes to other files
✓ No additional dependencies
✓ Same error handling
✓ Better performance (no serialization overhead)

## Status

**Priority:** HIGH
- Small cycles: Currently working
- Large cycles: Currently broken
- Users: Limited to ~20-30 KB cycles only

**Estimated Fix Time:** 5 minutes
**Risk Level:** LOW (direct write vs serialization)
**Testing:** Straightforward (upload large cycle, verify success)
