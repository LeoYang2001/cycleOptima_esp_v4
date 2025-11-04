// cycle.c
#include "cycle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cycle";

//GLOBAL VARIABLE
static uint64_t phase_start_us = 0;
static bool cycle_running = false;
static TaskHandle_t monitor_task_handle = NULL;
static const char *current_phase_name = "N/A";

// ------------------ PIN + SHADOW ------------------
static int gpio_shadow[NUM_COMPONENTS];
static const gpio_num_t all_pins[NUM_COMPONENTS] = {
    RETRACTOR_PIN, DETERGENT_VALVE_PIN, COLD_VALVE_PIN, DRAIN_PUMP_PIN,
    HOT_VALVE_PIN, SOFT_VALVE_PIN, MOTOR_ON_PIN, MOTOR_DIRECTION_PIN
};

// ------------------ PHASE RUN CONTEXT ------------------
// we want to be able to cancel timers later
#define MAX_EVENTS_PER_PHASE 24

typedef struct {
    TimelineEvent events[MAX_EVENTS_PER_PHASE];
    esp_timer_handle_t timers[MAX_EVENTS_PER_PHASE];
    size_t num_events;
    size_t remaining_events;     
    bool   active;
} PhaseRunContext;

static PhaseRunContext g_phase_ctx;   // current / latest phase

// ------------------------------------------------------------
// Debug task: logs all GPIO pin states every 100 ms
// ------------------------------------------------------------
static void gpio_monitor_task(void *pvParameters)
{
    while (cycle_running) {
        uint64_t now_us = esp_timer_get_time();
        double elapsed_ms = (now_us - phase_start_us) / 1000.0;

        printf("[%.3f s] %s GPIO STATUS → ", elapsed_ms / 1000.0,current_phase_name);
        for (int i = 0; i < NUM_COMPONENTS; i++) {
            printf("%d:%d ", all_pins[i], gpio_shadow[i]);
        }
        printf("\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    printf("[Monitor stopped]\n");
    vTaskDelete(NULL);
}

void start_gpio_monitor(void)
{
    if (monitor_task_handle) return;  // already running

    phase_start_us = esp_timer_get_time();

    xTaskCreate(gpio_monitor_task, "gpio_monitor", 3072, NULL, 1, &monitor_task_handle);
}

void stop_gpio_monitor(void)
{
    if (!monitor_task_handle) return;

    // Give the task a moment to finish printing
    vTaskDelay(pdMS_TO_TICKS(150));

    monitor_task_handle = NULL;
}

// ------------------------- GPIO INIT -------------------------
void init_all_gpio(void)
{
    for (int i = 0; i < NUM_COMPONENTS; i++) {
        gpio_reset_pin(all_pins[i]);
        gpio_set_direction(all_pins[i], GPIO_MODE_OUTPUT);
        gpio_set_level(all_pins[i], 1);  // active-low → OFF
        gpio_shadow[i] = 1;
    }

    ESP_LOGI(TAG, "All component GPIOs initialized to OFF (active-low).");
}

// ------------------------- COMP → PIN MAP -------------------------
typedef struct {
    const char *compId;
    gpio_num_t  pin;
} ComponentPinMap;

static const ComponentPinMap COMPONENT_PIN_MAP[] = {
    { "Retractor",        RETRACTOR_PIN },
    { "Cold Valve",       COLD_VALVE_PIN },
    { "Detergent Valve",  DETERGENT_VALVE_PIN },
    { "Drain Pump",       DRAIN_PUMP_PIN },
    { "Hot Valve",        HOT_VALVE_PIN },
    { "Soft Valve",       SOFT_VALVE_PIN },
    { "Motor On",         MOTOR_ON_PIN },
    { "Motor Direction",  MOTOR_DIRECTION_PIN }
};

static const size_t COMPONENT_PIN_MAP_LEN =
    sizeof(COMPONENT_PIN_MAP) / sizeof(COMPONENT_PIN_MAP[0]);

static gpio_num_t resolve_pin(const char *compId)
{
    for (size_t i = 0; i < COMPONENT_PIN_MAP_LEN; i++) {
        if (strcmp(compId, COMPONENT_PIN_MAP[i].compId) == 0) {
            return COMPONENT_PIN_MAP[i].pin;
        }
    }
    return GPIO_NUM_NC;
}

// ------------------------- TIMELINE BUILDER -------------------------
size_t build_timeline_from_phase(const Phase *phase,
                                 TimelineEvent *out_events,
                                 size_t max_events)
{
    size_t idx = 0;

    for (size_t i = 0; i < phase->num_components; i++) {
        const PhaseComponent *c = &phase->components[i];
        gpio_num_t pin = resolve_pin(c->compId);
        if (pin == GPIO_NUM_NC) {
            ESP_LOGW(TAG, "Unknown compId: %s", c->compId);
            continue;
        }

        // ON event
        if (idx < max_events) {
            out_events[idx].fire_time_us = (uint64_t)(phase->start_time_ms + c->start_ms) * 1000ULL;
            out_events[idx].type         = EVENT_ON;
            out_events[idx].pin          = pin;
            idx++;
        }

        // OFF event
        if (idx < max_events) {
            uint32_t off_ms = phase->start_time_ms + c->start_ms + c->duration_ms;
            out_events[idx].fire_time_us = (uint64_t)off_ms * 1000ULL;
            out_events[idx].type         = EVENT_OFF;
            out_events[idx].pin          = pin;
            idx++;
        }
    }

    return idx;
}

// ------------------------- TIMER CALLBACK -------------------------
static void event_timer_cb(void *arg)
{
    TimelineEvent *ev = (TimelineEvent *)arg;
    if (ev->pin == GPIO_NUM_NC) return;

    int level_to_set = (ev->type == EVENT_ON) ? 0 : 1;  // active-low
    gpio_set_level(ev->pin, level_to_set);

    // update shadow
    for (int i = 0; i < NUM_COMPONENTS; i++) {
        if (all_pins[i] == ev->pin) {
            gpio_shadow[i] = level_to_set;
            break;
        }
    }
    // ---- mark event done ----
    if (g_phase_ctx.remaining_events > 0) {
        g_phase_ctx.remaining_events--;
        if (g_phase_ctx.remaining_events == 0) {
            // all scheduled events have fired
            g_phase_ctx.active = false;
            ESP_LOGI(TAG, "Phase finished (all events fired).");
        }
    }
}

// ------------------------------------------------------------
// PUBLIC: run one phase with esp_timers, but keep handles
// ------------------------------------------------------------
void run_phase_with_esp_timer(const Phase *phase)
{
    // clear previous context
    memset(&g_phase_ctx, 0, sizeof(g_phase_ctx));
    g_phase_ctx.active = true;

    //set current phase name
      current_phase_name = phase->name ? phase->name : "Unknown"; 
      
    // build timeline into context
    size_t n = build_timeline_from_phase(phase,
                                         g_phase_ctx.events,
                                         MAX_EVENTS_PER_PHASE);
    g_phase_ctx.num_events = n;
    g_phase_ctx.remaining_events = n;

    uint64_t base_us = esp_timer_get_time();
    phase_start_us = base_us;   // so monitor prints from this phase start

    for (size_t i = 0; i < n; i++) {
        const TimelineEvent *ev = &g_phase_ctx.events[i];

        // create timer
        const esp_timer_create_args_t args = {
            .callback = event_timer_cb,
            .arg = (void *)ev,
            .name = "cycle_evt"
        };

        esp_timer_handle_t tmr;
        esp_err_t err = esp_timer_create(&args, &tmr);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_timer_create failed: %s", esp_err_to_name(err));
            continue;
        }

        // compute delay
        uint64_t now_us   = esp_timer_get_time();
        uint64_t elapsed  = now_us - base_us;
        uint64_t delay_us;
        if (ev->fire_time_us > elapsed) {
            delay_us = ev->fire_time_us - elapsed;
        } else {
            delay_us = 1000; // late: fire asap
        }

        // start
        esp_timer_start_once(tmr, delay_us);

        // store handle so we can cancel later
        g_phase_ctx.timers[i] = tmr;
    }

    ESP_LOGI(TAG, "Scheduled %zu events for phase %s", n, phase->name);
}

// ------------------------------------------------------------
// PUBLIC: skip/cancel current phase
// this can be called from a sensor task when condition is met
// ------------------------------------------------------------
void cycle_skip_current_phase(bool force_off_all)
{
    if (!g_phase_ctx.active) {
        return;
    }

    // stop all pending timers
    for (size_t i = 0; i < g_phase_ctx.num_events; i++) {
        if (g_phase_ctx.timers[i]) {
            esp_timer_stop(g_phase_ctx.timers[i]);
            esp_timer_delete(g_phase_ctx.timers[i]);
            g_phase_ctx.timers[i] = NULL;
        }
    }

    if (force_off_all) {
        // turn OFF everything (active-low → 1)
        for (int i = 0; i < NUM_COMPONENTS; i++) {
            gpio_set_level(all_pins[i], 1);
            gpio_shadow[i] = 1;
        }
    }

    g_phase_ctx.active = false;
    ESP_LOGW(TAG, "Current phase skipped/cancelled.");
}


void run_cycle(Phase *phases, size_t num_phases)
{
    cycle_running = true;
    start_gpio_monitor();

    for (size_t i = 0; i < num_phases; i++) {
        Phase *p = &phases[i];

        ESP_LOGI(TAG, "=== Running phase %d: %s ===", (int)i, p->name);
        run_phase_with_esp_timer(p);

        while (g_phase_ctx.active) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    ESP_LOGI(TAG, "=== Cycle completed ===");

    cycle_running = false;  // stop the monitor loop
    stop_gpio_monitor();
}