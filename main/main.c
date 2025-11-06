// main.c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"


#include "cycle.h"
#include "fs.h"
#include "wifi_sta.h"
#include "ws_cycle.h"


static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "=== CycleOptima ESP V4 ===");

    // 1) hardware ready
    init_all_gpio();

    // 2) mount SPIFFS
    if (fs_init_spiffs() != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS init failed");
        // we can still continue for websocket-only testing
    }

    // 3) try to load existing cycle.json, but DO NOT run it
    {
        char *json_str = fs_read_file("/spiffs/cycle.json");
        if (json_str) {
            if (cycle_load_from_json_str(json_str) == ESP_OK) {
                ESP_LOGI(TAG, "Loaded cycle.json at boot (IDLE)");
            } else {
                ESP_LOGW(TAG, "cycle.json exists but failed to parse");
            }
            free(json_str);
        } else {
            ESP_LOGI(TAG, "No /spiffs/cycle.json at boot, staying IDLE");
        }
    }

    // 4) connect to WiFi (manual SSID/PASS in wifi_sta.c)
    if (wifi_sta_init_and_connect() != ESP_OK) {
        ESP_LOGW(TAG, "WiFi init/connection failed, websocket may not be reachable");
    }

    // 5) start websocket that controls the flow
    if (ws_cycle_start() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start ws_cycle server");
    }

    // 6) run forever; all control comes from websocket
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
