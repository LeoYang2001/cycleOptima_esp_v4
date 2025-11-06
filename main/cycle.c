// cycle.c
#include "cycle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fs.h"
#include "cJSON.h"

static const char *TAG = "cycle";


// how many motor-config-bearing components we can handle
#define MAX_MOTOR_CONFIGS  32
#define MAX_MOTOR_STEPS    128
#define MAX_PHASES         16
#define MAX_COMPONENTS_PER_PHASE 16

static MotorConfig     g_motor_cfg_pool[MAX_MOTOR_CONFIGS];
static MotorPatternStep g_motor_steps_pool[MAX_MOTOR_STEPS];
static size_t g_motor_cfg_used  = 0;
static size_t g_motor_steps_used = 0;

//GLOBAL VARIABLE
static uint64_t phase_start_us = 0;
static bool cycle_running = false;
static TaskHandle_t monitor_task_handle = NULL;
static const char *current_phase_name = "N/A";

// Global state for loaded cycle (for cycle_load_from_json_str + cycle_run_loaded_cycle)
static Phase g_phases[MAX_PHASES];
static PhaseComponent g_components_pool[MAX_PHASES * MAX_COMPONENTS_PER_PHASE];
static size_t g_num_phases = 0;

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

esp_err_t load_cycle_from_json_str(const char *json_str,
                                   Phase *phases,
                                   size_t max_phases,
                                   PhaseComponent *components_pool,
                                   size_t max_components_per_phase,
                                   size_t *out_num_phases)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse error");
        return ESP_FAIL;
    }

    cJSON *phases_arr = cJSON_GetObjectItem(root, "phases");
    if (!cJSON_IsArray(phases_arr)) {
        ESP_LOGE(TAG, "'phases' is missing or not an array");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    size_t num_phases = cJSON_GetArraySize(phases_arr);
    if (num_phases > max_phases) {
        num_phases = max_phases;
    }

    for (size_t pi = 0; pi < num_phases; pi++) {
        cJSON *pjson = cJSON_GetArrayItem(phases_arr, pi);
        Phase *p = &phases[pi];

        cJSON *id        = cJSON_GetObjectItem(pjson, "id");
        cJSON *name      = cJSON_GetObjectItem(pjson, "name");
        cJSON *color     = cJSON_GetObjectItem(pjson, "color");
        cJSON *startTime = cJSON_GetObjectItem(pjson, "startTime");
        cJSON *components= cJSON_GetObjectItem(pjson, "components");

        p->id   = id   ? id->valuestring   : NULL;
        p->name = name ? name->valuestring : NULL;
        p->color= color? color->valuestring: NULL;
        p->start_time_ms = startTime ? (uint32_t)startTime->valueint : 0;

        size_t comp_count = cJSON_IsArray(components) ? cJSON_GetArraySize(components) : 0;
        if (comp_count > max_components_per_phase) {
            comp_count = max_components_per_phase;
        }

        PhaseComponent *phase_comps = &components_pool[pi * max_components_per_phase];
        p->components = phase_comps;
        p->num_components = comp_count;

        for (size_t ci = 0; ci < comp_count; ci++) {
            cJSON *cjson = cJSON_GetArrayItem(components, ci);
            PhaseComponent *c = &phase_comps[ci];

            cJSON *cid      = cJSON_GetObjectItem(cjson, "id");
            cJSON *label    = cJSON_GetObjectItem(cjson, "label");
            cJSON *start    = cJSON_GetObjectItem(cjson, "start");
            cJSON *compId   = cJSON_GetObjectItem(cjson, "compId");
            cJSON *duration = cJSON_GetObjectItem(cjson, "duration");
            cJSON *motorCfg = cJSON_GetObjectItem(cjson, "motorConfig");

            c->id          = cid      ? cid->valuestring      : NULL;
            c->label       = label    ? label->valuestring    : NULL;
            c->compId      = compId   ? compId->valuestring   : NULL;
            c->start_ms    = start    ? (uint32_t)start->valueint    : 0;
            c->duration_ms = duration ? (uint32_t)duration->valueint : 0;
            c->has_motor   = false;
            c->motor_cfg   = NULL;

            // optional motorConfig
           if (motorCfg && !cJSON_IsNull(motorCfg)) {
                // make sure we have room
                if (g_motor_cfg_used < MAX_MOTOR_CONFIGS) {
                    MotorConfig *mc = &g_motor_cfg_pool[g_motor_cfg_used++];
                    memset(mc, 0, sizeof(MotorConfig));

                    // repeatTimes
                    cJSON *repeatTimes  = cJSON_GetObjectItem(motorCfg, "repeatTimes");
                    mc->repeat_times = repeatTimes ? repeatTimes->valueint : 1;

                    // runningStyle (optional)
                    cJSON *runningStyle = cJSON_GetObjectItem(motorCfg, "runningStyle");
                    mc->running_style = runningStyle ? runningStyle->valuestring : NULL;

                    // pattern array
                    cJSON *pattern = cJSON_GetObjectItem(motorCfg, "pattern");
                    if (pattern && cJSON_IsArray(pattern)) {
                        int pattern_len = cJSON_GetArraySize(pattern);
                        if (pattern_len > 0) {
                            // remember where this motor's steps start in the global steps pool
                            size_t steps_start = g_motor_steps_used;
                            for (int si = 0; si < pattern_len; si++) {
                                if (g_motor_steps_used >= MAX_MOTOR_STEPS) {
                                    break;
                                }
                                cJSON *step_json = cJSON_GetArrayItem(pattern, si);
                                MotorPatternStep *step = &g_motor_steps_pool[g_motor_steps_used++];

                                cJSON *stepTime  = cJSON_GetObjectItem(step_json, "stepTime");
                                cJSON *pauseTime = cJSON_GetObjectItem(step_json, "pauseTime");
                                cJSON *direction = cJSON_GetObjectItem(step_json, "direction");

                                step->step_time_ms  = stepTime  ? (uint32_t)stepTime->valueint  : 1000;
                                step->pause_time_ms = pauseTime ? (uint32_t)pauseTime->valueint : 0;
                                step->direction     = direction ? direction->valuestring        : "cw";
                            }

                            // now point the motor config to its slice of steps
                            mc->pattern     = &g_motor_steps_pool[steps_start];
                            mc->pattern_len = g_motor_steps_used - steps_start;
                        }
                    }

                    // finally hook this component to this motor config
                    c->has_motor = true;
                    c->motor_cfg = mc;
                } else {
                    ESP_LOGW(TAG, "motorConfig present but motor cfg pool is full");
                }
            }
        }
    }

    *out_num_phases = num_phases;

    // NOTE: we didn't free root because we are borrowing strings
    // for quick prototyping that's fine

    return ESP_OK;
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
    { "Motor",         MOTOR_ON_PIN },
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

// Expand a motor component into timeline events.
// Returns how many events were written into out_events.
static size_t append_motor_events(const PhaseComponent *c,
                                  uint32_t phase_base_ms,
                                  TimelineEvent *out_events,
                                  size_t max_events)
{
    if (!c->motor_cfg) return 0;

    MotorConfig *mc = c->motor_cfg;
    size_t written = 0;
    uint32_t t_ms = c->start_ms;  // time offset inside this phase

    for (int r = 0; r < mc->repeat_times; r++) {
        for (size_t p = 0; p < mc->pattern_len; p++) {
            MotorPatternStep *step = &mc->pattern[p];

            // 1) set direction for this step
            if (written < max_events) {
                int dir_level = 0; // default: cw = 0
                if (step->direction && strcmp(step->direction, "ccw") == 0) {
                    dir_level = 1;
                }

                out_events[written].fire_time_us =
                    (uint64_t)(phase_base_ms + t_ms) * 1000ULL;
                out_events[written].type  = EVENT_ON;
                out_events[written].pin   = MOTOR_DIRECTION_PIN;
                out_events[written].level = dir_level;
                written++;
            }

            // 2) motor ON (active-low → 0)
            if (written < max_events) {
                out_events[written].fire_time_us =
                    (uint64_t)(phase_base_ms + t_ms) * 1000ULL;
                out_events[written].type  = EVENT_ON;
                out_events[written].pin   = MOTOR_ON_PIN;
                out_events[written].level = 0;
                written++;
            }

            // 3) motor OFF after stepTime
            if (written < max_events) {
                uint32_t off_ms = t_ms + step->step_time_ms;
                out_events[written].fire_time_us =
                    (uint64_t)(phase_base_ms + off_ms) * 1000ULL;
                out_events[written].type  = EVENT_OFF;
                out_events[written].pin   = MOTOR_ON_PIN;
                out_events[written].level = 1;    // active-low OFF
                written++;
            }

            // 4) advance time by step + pause
            t_ms += step->step_time_ms + step->pause_time_ms;
        }
    }

    return written;
}

// ------------------------- TIMELINE BUILDER -------------------------
size_t build_timeline_from_phase(const Phase *phase,
                                 TimelineEvent *out_events,
                                 size_t max_events)
{
    size_t idx = 0;

    for (size_t i = 0; i < phase->num_components && idx < max_events; i++) {
        const PhaseComponent *c = &phase->components[i];

        // motor branch
        if (c->has_motor && c->motor_cfg != NULL) {
            size_t added = append_motor_events(
                c,
                phase->start_time_ms,
                &out_events[idx],
                max_events - idx
            );
            idx += added;
            continue;
        }

        // normal component branch
        gpio_num_t pin = resolve_pin(c->compId);
        if (pin == GPIO_NUM_NC) {
            ESP_LOGW(TAG, "Unknown compId: %s", c->compId);
            continue;
        }

        // ON
        if (idx < max_events) {
            out_events[idx].fire_time_us =
                (uint64_t)(phase->start_time_ms + c->start_ms) * 1000ULL;
            out_events[idx].type  = EVENT_ON;
            out_events[idx].pin   = pin;
            out_events[idx].level = 0;      // active-low ON
            idx++;
        }

        // OFF
        if (idx < max_events) {
            uint32_t off_ms = phase->start_time_ms + c->start_ms + c->duration_ms;
            out_events[idx].fire_time_us =
                (uint64_t)off_ms * 1000ULL;
            out_events[idx].type  = EVENT_OFF;
            out_events[idx].pin   = pin;
            out_events[idx].level = 1;      // active-low OFF
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

    gpio_set_level(ev->pin, ev->level);

    // update shadow
     for (int i = 0; i < NUM_COMPONENTS; i++) {
        if (all_pins[i] == ev->pin) {
            gpio_shadow[i] = ev->level;
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
bool cycle_is_running(void)
{
    return cycle_running;
}

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
    cycle_running = false;
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



esp_err_t cycle_load_from_json_str(const char *json_str)
{
    if (!json_str) {
        ESP_LOGE(TAG, "cycle_load_from_json_str: null json_str");
        return ESP_FAIL;
    }

    // reset motor pools and phase count
    g_motor_cfg_used = 0;
    g_motor_steps_used = 0;
    g_num_phases = 0;
    memset(g_phases, 0, sizeof(g_phases));
    memset(g_components_pool, 0, sizeof(g_components_pool));

    // Use existing parser
    esp_err_t ret = load_cycle_from_json_str(
        json_str,
        g_phases,
        MAX_PHASES,
        g_components_pool,
        MAX_COMPONENTS_PER_PHASE,
        &g_num_phases
    );

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Loaded %zu phases into RAM (IDLE)", g_num_phases);
    } else {
        ESP_LOGE(TAG, "Failed to load cycle from JSON");
    }

    return ret;
}


// Task to run the cycle in the background (non-blocking)
static void cycle_task(void *pvParameter)
{
    run_cycle(g_phases, g_num_phases);
    vTaskDelete(NULL);
}

void cycle_run_loaded_cycle(void)
{
    if (g_num_phases == 0) {
        ESP_LOGW(TAG, "cycle_run_loaded_cycle: no cycle loaded");
        return;
    }

    ESP_LOGI(TAG, "Running loaded cycle (%zu phases) in background task", g_num_phases);
    
    // Create a background task to run the cycle so WebSocket stays responsive
    xTaskCreatePinnedToCore(
        cycle_task,           // task function
        "cycle_runner",       // task name
        4096,                 // stack size
        NULL,                 // parameter
        5,                    // priority (above normal)
        NULL,                 // task handle (not needed for cleanup)
        0                     // core 0
    );
}