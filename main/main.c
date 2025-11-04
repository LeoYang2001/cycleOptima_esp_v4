// main.c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "cycle.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "=== CycleOptima ESP Timer Scheduler Demo ===");

    init_all_gpio();


    Phase cycle_phases[] = {
        {
            .id = "1755269543284",
            .name = "phase1",
            .color = "4ADE80",
            .start_time_ms = 0,
            .components = (PhaseComponent[]) {
                { "1762273103956", "Standard Retractor Cycle", "Retractor", 0, 10000, false },
                { "1762273110068", "Cold Valve2", "Detergent Valve", 1000, 10000, false },
                { "1762273123765", "Hot Valve", "Hot Valve", 2000, 10000, false }
            },
            .num_components = 3
        },
        {
            .id = "1762273150291",
            .name = "phase2",
            .color = "F59E0B",
            .start_time_ms = 0,
            .components = (PhaseComponent[]) {
                { "1762273155825", "Cold Valve1", "Cold Valve", 0, 6000, false }
            },
            .num_components = 1
        }
    };
    size_t num_phases = sizeof(cycle_phases) / sizeof(cycle_phases[0]);

    // Run the full cycle sequentially
    run_cycle(cycle_phases, num_phases);

}
