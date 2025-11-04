// main.c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "cycle.h"
#include "fs.h"
#include "wifi_sta.h"

static const char *TAG = "main";

// move big buffers off the stack
static Phase          s_phases[15];              // up to 15 phases
static PhaseComponent s_components_pool[15 * 20]; 

void app_main(void)
{
    ESP_LOGI(TAG, "=== CycleOptima ESP Timer Scheduler Demo ===");

      if (wifi_sta_init_and_connect() != ESP_OK) {
        ESP_LOGE("main", "wifi init failed");
    }

    init_all_gpio();

    if (fs_init_spiffs() != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS init failed");
        return;
    }

    char *json_str = fs_read_file("/spiffs/cycle.json");
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to read /spiffs/cycle.json");
        return;
    }

    size_t num_phases = 0;
    if (load_cycle_from_json_str(json_str,
                                 s_phases,
                                 15,
                                 s_components_pool,
                                 20,
                                 &num_phases) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        free(json_str);
        return;
    }
    free(json_str);

    run_cycle(s_phases, num_phases);

}
