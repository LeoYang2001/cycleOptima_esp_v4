// ws_cycle.h
#pragma once

#include "esp_err.h"
#include <stdint.h>

// Start a websocket-capable HTTP server on /ws.
// Call this after Wi-Fi is up.
esp_err_t ws_cycle_start(void);

// Get the WebSocket server port (for logging after IP is obtained)
uint16_t ws_cycle_get_port(void);
