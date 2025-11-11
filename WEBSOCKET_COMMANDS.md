# WebSocket Commands Documentation

All commands are sent as JSON over the WebSocket connection at `ws://<ESP_IP>:8080/ws`

---

## 1. `write_json` - Load Cycle Definition

**Purpose:** Load a new cycle definition into the device and save it to SPIFFS storage.

**JSON Format:**
```json
{
  "action": "write_json",
  "data": {
    "phases": [
      {
        "id": "phase1",
        "name": "Wash",
        "color": "#0000FF",
        "startTime": 0,
        "components": [
          {
            "id": "comp1",
            "label": "Motor",
            "compId": "motor",
            "start": 0,
            "duration": 5000,
            "motorConfig": {
              "repeatTimes": 1,
              "runningStyle": "Single Direction",
              "pattern": [
                {"stepTime": 100, "pauseTime": 50, "direction": "cw"},
                {"stepTime": 100, "pauseTime": 50, "direction": "ccw"}
              ]
            }
          }
        ]
      }
    ]
  }
}
```

**Response:**
```json
"ok: json written and loaded"
```

**Error Responses:**
```json
"error: missing data for write_json"
"error: json written but failed to load"
```

**Notes:**
- Stops any currently running cycle before loading new one
- Saves cycle definition to `/spiffs/cycle.json`
- Does NOT automatically start the cycle

---

## 2. `start_cycle` - Start Cycle Execution

**Purpose:** Begin executing the currently loaded cycle.

**JSON Format:**
```json
{
  "action": "start_cycle"
}
```

**Response:**
```json
"ok: starting cycle"
```

**Error Responses:**
```json
"error: cycle already running"
```

**Notes:**
- Must have a cycle loaded via `write_json` first
- Cycle executes in background; telemetry updates stream continuously

---

## 3. `stop_cycle` - Stop Cycle Execution

**Purpose:** Immediately stop the currently running cycle and turn off all GPIO pins.

**JSON Format:**
```json
{
  "action": "stop_cycle"
}
```

**Response:**
```json
"ok: cycle stopped"
```

**Notes:**
- Stops all active timers
- Forces all GPIO pins to OFF (inactive state)
- No error if cycle not running

---

## 4. `skip_phase` - Skip to Next Phase

**Purpose:** Finish current phase immediately and advance to the next phase.

**JSON Format:**
```json
{
  "action": "skip_phase"
}
```

**Response:**
```json
"ok: phase skipped"
```

**Notes:**
- Only works when cycle is running
- Turns off all GPIO pins during transition
- Automatically advances to next phase in sequence

---

## 5. `skip_to_phase` - Jump to Specific Phase

**Purpose:** Jump directly to a specific phase by index.

**JSON Format:**
```json
{
  "action": "skip_to_phase",
  "index": 2
}
```

**Response:**
```json
"ok: skipping to phase"
```

**Error Responses:**
```json
"error: missing or invalid index for skip_to_phase"
```

**Notes:**
- `index` is 0-based (0 = first phase, 1 = second phase, etc.)
- Only works when cycle is running
- Turns off all GPIO pins during transition

---

## 6. `toggle_gpio` - Manually Control GPIO Pin

**Purpose:** Directly set a GPIO pin HIGH (1) or LOW (0) regardless of cycle state.

**JSON Format:**
```json
{
  "action": "toggle_gpio",
  "pin": 7,
  "state": 1
}
```

**Response:**
```json
"ok: GPIO 7 set to 1"
```

**Error Responses:**
```json
"error: missing or invalid pin number"
"error: missing or invalid state (0 or 1)"
```

**GPIO Pin Mapping:**
| Pin | Component | Name |
|-----|-----------|------|
| 7 | RETRACTOR | Retractor Solenoid |
| 8 | DETERGENT_VALVE | Detergent Valve |
| 5 | COLD_VALVE | Cold Water Valve |
| 19 | DRAIN_PUMP | Drain Pump |
| 9 | HOT_VALVE | Hot Water Valve |
| 18 | SOFT_VALVE | Fabric Softener Valve |
| 4 | MOTOR_ON | Motor Enable |
| 10 | MOTOR_DIRECTION | Motor Direction |

**Notes:**
- Works when cycle is running OR idle
- Immediately updates `gpio_shadow` so telemetry reflects the change
- `state=1` sets pin HIGH (relay activated)
- `state=0` sets pin LOW (relay deactivated)
- Next telemetry update (100ms) will broadcast the new GPIO state

---

## Telemetry Stream (Automatic Broadcasts)

The device automatically broadcasts telemetry data every 100ms to all connected clients.

**Telemetry JSON Format:**
```json
{
  "type": "telemetry",
  "packet_timestamp_ms": 1234567,
  "gpio": [
    {"pin": 7, "state": 1},
    {"pin": 8, "state": 0},
    {"pin": 5, "state": 1},
    {"pin": 19, "state": 0},
    {"pin": 9, "state": 0},
    {"pin": 18, "state": 0},
    {"pin": 4, "state": 1},
    {"pin": 10, "state": 0}
  ],
  "sensors": {
    "rpm": 1250,
    "pressure_freq": 2450.5,
    "sensor_error": false
  },
  "cycle": {
    "cycle_running": true,
    "current_phase_index": 1,
    "current_phase_name": "Wash",
    "total_phases": 5,
    "phase_elapsed_ms": 3200,
    "phase_total_duration_ms": 5000,
    "cycle_start_time_ms": 0
  },
  "cycle_data": [
    {
      "id": "phase1",
      "name": "Wash",
      "color": "#0000FF",
      "start_time_ms": 0,
      "components": [
        {
          "id": "comp1",
          "label": "Motor",
          "compId": "motor",
          "start_ms": 0,
          "duration_ms": 5000,
          "has_motor": true
        }
      ]
    }
  ]
}
```

---

## Usage Examples

### Example 1: Load and Start a Simple Cycle

```json
{"action": "write_json", "data": {"phases": [{"id": "p1", "name": "Test", "color": "#FF0000", "startTime": 0, "components": [{"id": "c1", "label": "Pump", "compId": "pump", "start": 0, "duration": 2000}]}]}}
```

Then:
```json
{"action": "start_cycle"}
```

### Example 2: Skip to Phase 2 While Running

```json
{"action": "skip_to_phase", "index": 2}
```

### Example 3: Toggle Retractor ON

```json
{"action": "toggle_gpio", "pin": 7, "state": 1}
```

### Example 4: Toggle All Relays OFF

```json
{"action": "toggle_gpio", "pin": 7, "state": 0}
{"action": "toggle_gpio", "pin": 8, "state": 0}
{"action": "toggle_gpio", "pin": 5, "state": 0}
{"action": "toggle_gpio", "pin": 19, "state": 0}
{"action": "toggle_gpio", "pin": 9, "state": 0}
{"action": "toggle_gpio", "pin": 18, "state": 0}
```

### Example 5: Stop Cycle

```json
{"action": "stop_cycle"}
```

---

## Testing with Command Line Tools

### Using `websocat` (Install: `cargo install websocat`)

```bash
# Connect to WebSocket
websocat ws://192.168.1.100:8080/ws

# Send command (paste into websocat and press Enter)
{"action": "start_cycle"}
{"action": "toggle_gpio", "pin": 7, "state": 1}
```

### Using Python

```python
import asyncio
import websockets
import json

async def send_command(uri, action_data):
    async with websockets.connect(uri) as websocket:
        await websocket.send(json.dumps(action_data))
        response = await websocket.recv()
        print(f"Response: {response}")

# Start cycle
asyncio.run(send_command("ws://192.168.1.100:8080/ws", {"action": "start_cycle"}))

# Toggle GPIO
asyncio.run(send_command("ws://192.168.1.100:8080/ws", {"action": "toggle_gpio", "pin": 7, "state": 1}))
```

### Using Browser Console

```javascript
const ws = new WebSocket("ws://192.168.1.100:8080/ws");

ws.onmessage = (event) => {
  const data = JSON.parse(event.data);
  console.log("Telemetry:", data);
};

ws.onopen = () => {
  ws.send(JSON.stringify({"action": "start_cycle"}));
};
```

---

## Command Summary Table

| Command | Parameters | Purpose |
|---------|------------|---------|
| `write_json` | `data` | Load cycle definition |
| `start_cycle` | None | Start cycle |
| `stop_cycle` | None | Stop cycle |
| `skip_phase` | None | Skip to next phase |
| `skip_to_phase` | `index` | Jump to phase |
| `toggle_gpio` | `pin`, `state` | Control GPIO pin |

---

## Notes

- All commands return immediately with status response
- Cycle execution happens in background task
- Telemetry streams automatically at 100ms intervals
- GPIO shadow state updates immediately on `toggle_gpio` command
- All GPIO pins are active-LOW (1 = relay ON, 0 = relay OFF)
