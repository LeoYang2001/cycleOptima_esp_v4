// wifi_sta.h
#pragma once

#include <stdbool.h>

#include "esp_err.h"

// call once from app_main
esp_err_t wifi_sta_init_and_connect(void);

// optionally get current state later
bool wifi_sta_is_connected(void);