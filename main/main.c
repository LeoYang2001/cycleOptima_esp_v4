// main.c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"


#include "cycle.h"
#include "fs.h"
#include "wifi_sta.h"
#include "ws_cycle.h"
#include "telemetry.h"


static const char *TAG = "main";


// this task is responsible for: connect Wi-Fi -> start websocket
static void net_task(void *pvParam)
{
    ESP_LOGI(TAG, "[net_task] starting Wi-Fi bring-up...");

    // 1) connect to Wi-Fi (block/retry until success, or bail out)
    // you can make this a loop if you want auto-retry
    esp_err_t err = wifi_sta_init_and_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[net_task] Wi-Fi failed, websocket will NOT start");
        // stay alive so system doesn’t crash; you could also retry here
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }

    ESP_LOGI(TAG, "[net_task] Wi-Fi connected, starting websocket...");

    // 2) ONLY AFTER WIFI is up, start websocket/server
    err = ws_cycle_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[net_task] Failed to start ws_cycle server");
        // same idea: stay alive or retry
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }

    ESP_LOGI(TAG, "[net_task] Websocket server started.");

    // if your websocket runs in its own task/event loop, this task
    // doesn’t have to do anything else
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== CycleOptima ESP V4 ===");

    // 1) hardware ready
    init_all_gpio();

    // 2) start telemetry system (gathers GPIO, sensors, cycle info)
    telemetry_init(100);  // update every 100ms

    // 3) mount SPIFFS
    if (fs_init_spiffs() != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS init failed");
        // we can still continue for websocket-only testing
    }

    // 4) try to load existing cycle.json, but DO NOT run it yet
    {
        char *json_str = fs_read_file("/spiffs/cycle.json");
        if (json_str) {
            if (cycle_load_from_json_str(json_str) == ESP_OK) {
                ESP_LOGI(TAG, "Loaded /spiffs/cycle.json at boot (IDLE)");
            } else {
                ESP_LOGW(TAG, "cycle.json exists but failed to parse");
            }
            free(json_str);
        } else {
            ESP_LOGI(TAG, "No /spiffs/cycle.json at boot, staying IDLE");
        }
    }

    // 5) start ONE task that will: Wi-Fi -> websocket
    xTaskCreate(
        net_task,
        "net_task",
        4096,
        NULL,
        5,
        NULL
    );

    // 6) main loop just idles; control comes from websocket
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}