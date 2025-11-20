// cycle.c
    #include "cycle.h"
    #include "esp_log.h"
    #include "esp_timer.h"
    #include "driver/gpio.h"
    #include <string.h>

    #include "freertos/FreeRTOS.h"
    #include "freertos/task.h"
    #include "freertos/semphr.h"

    #include "fs.h"
    #include "cJSON.h"
    #include "rpm_sensor.h"      // for rpm_sensor_reset(), rpm_sensor_get_rpm()
    #include "pressure_sensor.h" // for pressure_sensor_reset(), pressure_sensor_read_frequency()
    #include "ws_cycle.h"        // for ws_update_cycle_data_cache()

    static const char *TAG = "cycle";

// Keep track of parsed JSON root so we can free it when unloading
static cJSON *g_loaded_cycle_json = NULL;

    // Memory pools using limits defined in cycle.h
    static MotorConfig     g_motor_cfg_pool[MAX_MOTOR_CONFIGS];
    static MotorPatternStep g_motor_steps_pool[MAX_MOTOR_STEPS];
    static size_t g_motor_cfg_used  = 0;
    static size_t g_motor_steps_used = 0;

    // Sensor trigger pool (one per phase max)
    static SensorTrigger g_sensor_trigger_pool[MAX_SENSOR_TRIGGERS];
    static size_t g_sensor_trigger_used = 0;

//GLOBAL VARIABLE
uint64_t phase_start_us = 0;  // track phase start time (non-static for telemetry access)
bool cycle_running = false;  // non-static for telemetry access
const char *current_phase_name = "N/A";  // non-static for telemetry access
static int target_phase_index = -1;  // -1 means no skip, -2 means stop cycle, otherwise jump to this phase
int current_phase_index = 0;  // track which phase we're currently running (accessible to telemetry)

// Global state for loaded cycle (for cycle_load_from_json_str + cycle_run_loaded_cycle)
Phase g_phases[MAX_PHASES];  // non-static for telemetry/WebSocket access
static PhaseComponent g_components_pool[MAX_PHASES * MAX_COMPONENTS_PER_PHASE];
size_t g_num_phases = 0;  // non-static for telemetry access    // ------------------ PIN + SHADOW ------------------
    int gpio_shadow[NUM_COMPONENTS];
    const gpio_num_t all_pins[NUM_COMPONENTS] = {
        RETRACTOR_PIN, DETERGENT_VALVE_PIN, COLD_VALVE_PIN, DRAIN_PUMP_PIN,
        HOT_VALVE_PIN, SOFT_VALVE_PIN, MOTOR_ON_PIN, MOTOR_DIRECTION_PIN
    };

    // ------------------ PHASE RUN CONTEXT ------------------
    #define BATCH_SIZE 200  // Max timers to create at once (heap-friendly)
    
    typedef struct {
        TimelineEvent events[MAX_EVENTS_PER_PHASE];
        esp_timer_handle_t timers[BATCH_SIZE];  // Only store current batch timers
        size_t num_events;
        size_t remaining_events;     
        bool   active;
        
        // Batch tracking
        size_t current_batch_idx;       // Which batch are we on (0, 1, 2, ...)
        size_t batches_total;           // Total number of batches
        size_t next_batch_start_event;  // Index of first event in next batch
        esp_timer_handle_t batch_timer; // Timer that signals batch completion
        uint64_t batch_start_us;        // When this batch started (for timing)
        SemaphoreHandle_t batch_sem;    // semaphore used to notify task to load next batch
    } PhaseRunContext;

    static PhaseRunContext g_phase_ctx;   // current / latest phase

    esp_err_t load_cycle_from_json_str(const char *json_str,
                                    Phase *phases,
                                    size_t max_phases,
                                    PhaseComponent *components_pool,
                                    size_t max_components_per_phase,
                                    size_t *out_num_phases)
    {
        ESP_LOGI(TAG, "Parsing cycle JSON (length: %zu bytes)...", strlen(json_str));
        
        cJSON *root = cJSON_Parse(json_str);
        
        if (!root) {
            const char *error_ptr = cJSON_GetErrorPtr();
            if (error_ptr && error_ptr >= json_str) {
                size_t error_offset = error_ptr - json_str;
                ESP_LOGE(TAG, "JSON parse error at offset %zu", error_offset);
            } else {
                ESP_LOGE(TAG, "JSON parse error");
            }
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
            cJSON *startTime = cJSON_GetObjectItem(pjson, "startTime");
            cJSON *components= cJSON_GetObjectItem(pjson, "components");

            p->id   = id   ? id->valuestring   : NULL;
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
                cJSON *start    = cJSON_GetObjectItem(cjson, "start");
                cJSON *compId   = cJSON_GetObjectItem(cjson, "compId");
                cJSON *duration = cJSON_GetObjectItem(cjson, "duration");
                cJSON *motorCfg = cJSON_GetObjectItem(cjson, "motorConfig");

                c->id          = cid      ? cid->valuestring      : NULL;
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

                        // pattern array
                        cJSON *pattern = cJSON_GetObjectItem(motorCfg, "pattern");
                        if (pattern && cJSON_IsArray(pattern)) {
                            int pattern_len = cJSON_GetArraySize(pattern);
                            if (pattern_len > 0) {
                                ESP_LOGI(TAG, "Processing motor pattern with %d steps (repeat: %d), steps pool: %zu/%d", 
                                        pattern_len, mc->repeat_times, g_motor_steps_used, MAX_MOTOR_STEPS);
                                
                                // Calculate total steps needed including repeats
                                int total_steps_needed = pattern_len * mc->repeat_times;
                                
                                // remember where this motor's steps start in the global steps pool
                                size_t steps_start = g_motor_steps_used;
                                for (int si = 0; si < pattern_len; si++) {
                                    if (g_motor_steps_used >= MAX_MOTOR_STEPS) {
                                        ESP_LOGE(TAG, "Motor steps pool exhausted! Used: %zu, Max: %d. Pattern truncated at step %d/%d", 
                                                g_motor_steps_used, MAX_MOTOR_STEPS, si, pattern_len);
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
                                
                                ESP_LOGI(TAG, "Motor pattern stored: %zu steps from pool[%zu], total would be %d with repeats", 
                                        mc->pattern_len, steps_start, total_steps_needed);
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

            // Parse optional sensorTrigger for this phase
            cJSON *sensorTrigger = cJSON_GetObjectItem(pjson, "sensorTrigger");
            p->sensor_trigger = NULL;  // default: no trigger
            
            if (sensorTrigger && !cJSON_IsNull(sensorTrigger)) {
                if (g_sensor_trigger_used < MAX_SENSOR_TRIGGERS) {
                    SensorTrigger *st = &g_sensor_trigger_pool[g_sensor_trigger_used++];
                    memset(st, 0, sizeof(SensorTrigger));
                    
                    // Parse sensor type
                    cJSON *type = cJSON_GetObjectItem(sensorTrigger, "type");
                    const char *type_str = type ? type->valuestring : "RPM";
                    if (strcmp(type_str, "RPM") == 0) {
                        st->type = SENSOR_TYPE_RPM;
                    } else if (strcmp(type_str, "Pressure") == 0) {
                        st->type = SENSOR_TYPE_PRESSURE;
                    } else {
                        st->type = SENSOR_TYPE_UNKNOWN;
                    }
                    
                    // Parse threshold
                    cJSON *threshold = cJSON_GetObjectItem(sensorTrigger, "threshold");
                    st->threshold = threshold ? (uint32_t)threshold->valueint : 0;
                    
                    // Parse trigger direction
                    cJSON *triggerAbove = cJSON_GetObjectItem(sensorTrigger, "triggerAbove");
                    st->trigger_above = triggerAbove ? triggerAbove->type == cJSON_True : true;
                    
                    // Track that trigger hasn't fired yet
                    st->has_triggered = false;
                    
                    p->sensor_trigger = st;
                    ESP_LOGI(TAG, "Phase '%s': sensor trigger configured (type=%d, threshold=%u, above=%d)",
                             p->id ? p->id : "unknown", st->type, st->threshold, st->trigger_above);
                } else {
                    ESP_LOGW(TAG, "sensor_trigger pool full, ignoring trigger for phase '%s'", p->id ? p->id : "unknown");
                }
            }
        }

        *out_num_phases = num_phases;

        // Store the JSON root globally so we can free it later when unloading
        // The string pointers in structs are borrowed from this JSON tree
        g_loaded_cycle_json = root;

        return ESP_OK;
    }


    // ========== OPTIMIZED LOADING: Direct from cJSON tree (no re-parse) ==========
    // This function loads a cycle directly from an already-parsed cJSON tree,
    // avoiding the need to serialize and re-parse. Used for WebSocket uploads.
    esp_err_t load_cycle_from_cjson(cJSON *root_json)
    {
        if (!root_json) {
            ESP_LOGE(TAG, "load_cycle_from_cjson: null root_json");
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "Loading cycle directly from cJSON tree (no re-parse)...");

        // Free any previously loaded cycle
        cycle_unload();

        ESP_LOGI(TAG, "Pools reset. MAX_MOTOR_STEPS=%d, MAX_PHASES=%d", MAX_MOTOR_STEPS, MAX_PHASES);

        // Extract phases array from root
        cJSON *phases_arr = cJSON_GetObjectItem(root_json, "phases");
        if (!cJSON_IsArray(phases_arr)) {
            ESP_LOGE(TAG, "'phases' is missing or not an array in root");
            return ESP_FAIL;
        }

        size_t num_phases = cJSON_GetArraySize(phases_arr);
        if (num_phases > MAX_PHASES) {
            num_phases = MAX_PHASES;
        }

        // Parse each phase
        for (size_t pi = 0; pi < num_phases; pi++) {
            cJSON *pjson = cJSON_GetArrayItem(phases_arr, pi);
            Phase *p = &g_phases[pi];

            cJSON *id        = cJSON_GetObjectItem(pjson, "id");
            cJSON *startTime = cJSON_GetObjectItem(pjson, "startTime");
            cJSON *components= cJSON_GetObjectItem(pjson, "components");

            p->id   = id   ? id->valuestring   : NULL;
            p->start_time_ms = startTime ? (uint32_t)startTime->valueint : 0;

            size_t comp_count = cJSON_IsArray(components) ? cJSON_GetArraySize(components) : 0;
            if (comp_count > MAX_COMPONENTS_PER_PHASE) {
                comp_count = MAX_COMPONENTS_PER_PHASE;
            }

            PhaseComponent *phase_comps = &g_components_pool[pi * MAX_COMPONENTS_PER_PHASE];
            p->components = phase_comps;
            p->num_components = comp_count;

            // Parse each component
            for (size_t ci = 0; ci < comp_count; ci++) {
                cJSON *cjson = cJSON_GetArrayItem(components, ci);
                PhaseComponent *c = &phase_comps[ci];

                cJSON *cid      = cJSON_GetObjectItem(cjson, "id");
                cJSON *start    = cJSON_GetObjectItem(cjson, "start");
                cJSON *compId   = cJSON_GetObjectItem(cjson, "compId");
                cJSON *duration = cJSON_GetObjectItem(cjson, "duration");
                cJSON *motorCfg = cJSON_GetObjectItem(cjson, "motorConfig");

                c->id          = cid      ? cid->valuestring      : NULL;
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

                        // pattern array
                        cJSON *pattern = cJSON_GetObjectItem(motorCfg, "pattern");
                        if (pattern && cJSON_IsArray(pattern)) {
                            int pattern_len = cJSON_GetArraySize(pattern);
                            if (pattern_len > 0) {
                                ESP_LOGI(TAG, "Processing motor pattern with %d steps (repeat: %d), steps pool: %zu/%d", 
                                        pattern_len, mc->repeat_times, g_motor_steps_used, MAX_MOTOR_STEPS);
                                
                                // remember where this motor's steps start in the global steps pool
                                size_t steps_start = g_motor_steps_used;
                                for (int si = 0; si < pattern_len; si++) {
                                    if (g_motor_steps_used >= MAX_MOTOR_STEPS) {
                                        ESP_LOGE(TAG, "Motor steps pool exhausted! Used: %zu, Max: %d. Pattern truncated at step %d/%d", 
                                                g_motor_steps_used, MAX_MOTOR_STEPS, si, pattern_len);
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
                                
                                ESP_LOGI(TAG, "Motor pattern stored: %zu steps from pool[%zu]", 
                                        mc->pattern_len, steps_start);
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

            // Parse optional sensorTrigger for this phase
            cJSON *sensorTrigger = cJSON_GetObjectItem(pjson, "sensorTrigger");
            p->sensor_trigger = NULL;  // default: no trigger
            
            if (sensorTrigger && !cJSON_IsNull(sensorTrigger)) {
                if (g_sensor_trigger_used < MAX_SENSOR_TRIGGERS) {
                    SensorTrigger *st = &g_sensor_trigger_pool[g_sensor_trigger_used++];
                    memset(st, 0, sizeof(SensorTrigger));
                    
                    // Parse sensor type
                    cJSON *type = cJSON_GetObjectItem(sensorTrigger, "type");
                    const char *type_str = type ? type->valuestring : "RPM";
                    if (strcmp(type_str, "RPM") == 0) {
                        st->type = SENSOR_TYPE_RPM;
                    } else if (strcmp(type_str, "Pressure") == 0) {
                        st->type = SENSOR_TYPE_PRESSURE;
                    } else {
                        st->type = SENSOR_TYPE_UNKNOWN;
                    }
                    
                    // Parse threshold
                    cJSON *threshold = cJSON_GetObjectItem(sensorTrigger, "threshold");
                    st->threshold = threshold ? (uint32_t)threshold->valueint : 0;
                    
                    // Parse trigger direction
                    cJSON *triggerAbove = cJSON_GetObjectItem(sensorTrigger, "triggerAbove");
                    st->trigger_above = triggerAbove ? triggerAbove->type == cJSON_True : true;
                    
                    // Track that trigger hasn't fired yet
                    st->has_triggered = false;
                    
                    p->sensor_trigger = st;
                    ESP_LOGI(TAG, "Phase '%s': sensor trigger configured (type=%d, threshold=%u, above=%d)",
                             p->id ? p->id : "unknown", st->type, st->threshold, st->trigger_above);
                } else {
                    ESP_LOGW(TAG, "sensor_trigger pool full, ignoring trigger for phase '%s'", p->id ? p->id : "unknown");
                }
            }
        }

        g_num_phases = num_phases;

        // CRITICAL: Store the root JSON object so all borrowed string pointers remain valid
        // The phases and components contain string pointers (id, compId, etc.) that point
        // into this cJSON tree. We must keep the tree alive for the lifetime of the cycle.
        // It will be freed when cycle_unload() is called (when a new cycle loads or manually).
        g_loaded_cycle_json = root_json;

        ESP_LOGI(TAG, "Loaded %zu phases into RAM. Motor configs used: %zu, Motor steps used: %zu/%d", 
                g_num_phases, g_motor_cfg_used, g_motor_steps_used, MAX_MOTOR_STEPS);

        // Update WebSocket cycle_data cache with the newly loaded cycle
        ws_update_cycle_data_cache();

        return ESP_OK;
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
        size_t motor_events = 0;
        size_t regular_events = 0;

        ESP_LOGI(TAG, "Building timeline for phase '%s' with %zu components...", 
                 phase->id ? phase->id : "unnamed", phase->num_components);

        for (size_t i = 0; i < phase->num_components && idx < max_events; i++) {
            const PhaseComponent *c = &phase->components[i];

            // motor branch
            if (c->has_motor && c->motor_cfg != NULL) {
                size_t before_motor = idx;
                size_t added = append_motor_events(
                    c,
                    phase->start_time_ms,
                    &out_events[idx],
                    max_events - idx
                );
                idx += added;
                motor_events += added;
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

        // Summary log only - detailed event logging removed for performance
        ESP_LOGI(TAG, "Built timeline: %zu events (motor: %zu, regular: %zu)", 
                 idx, motor_events, idx - motor_events);

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

    // Called when a batch of timers completes. This callback now only signals the
    // cycle task (via semaphore) to perform deletion and creation of the next batch
    // in task context. Doing heavy allocation (esp_timer_create) inside the timer
    // task can cause heap pressure and fragmentation leading to ESP_ERR_NO_MEM.
    static void load_next_batch_timer_cb(void *arg)
    {
        (void)arg;
        if (g_phase_ctx.batch_sem) {
            BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            // We're in timer task context, use xSemaphoreGiveFromISR semantics
            xSemaphoreGiveFromISR(g_phase_ctx.batch_sem, &xHigherPriorityTaskWoken);
            // yield if needed
            if (xHigherPriorityTaskWoken == pdTRUE) {
                portYIELD_FROM_ISR();
            }
        } else {
            ESP_LOGW(TAG, "Batch timer fired but no batch semaphore present");
        }
    }

    // ------------------------------------------------------------
    // PUBLIC: run one phase with esp_timers using batching
    // For large event counts (up to 1600), load events in batches of 200
    // to avoid heap exhaustion from creating too many timer handles at once
    // ------------------------------------------------------------
    void run_phase_with_esp_timer(const Phase *phase)
    {
        // clear previous context
        memset(&g_phase_ctx, 0, sizeof(g_phase_ctx));
        // create semaphore used for signaling batch loads
        g_phase_ctx.batch_sem = xSemaphoreCreateBinary();
        if (!g_phase_ctx.batch_sem) {
            ESP_LOGW(TAG, "Failed to create batch semaphore, batching may be unsafe");
        }
        g_phase_ctx.active = true;

        //set current phase name
        current_phase_name = phase->id ? phase->id : "Unknown"; 
        
        
        // build timeline into context
        size_t n = build_timeline_from_phase(phase,
                                            g_phase_ctx.events,
                                            MAX_EVENTS_PER_PHASE);
        g_phase_ctx.num_events = n;
        g_phase_ctx.remaining_events = n;

        uint64_t base_us = esp_timer_get_time();
        phase_start_us = base_us;   // so monitor prints from this phase start
        g_phase_ctx.batch_start_us = base_us;

        // Calculate number of batches needed
        g_phase_ctx.batches_total = (n + BATCH_SIZE - 1) / BATCH_SIZE;
        g_phase_ctx.current_batch_idx = 0;

        ESP_LOGI(TAG, "Phase '%s': %zu events in %zu batches (batch_size=%u)", 
                 phase->id, n, g_phase_ctx.batches_total, BATCH_SIZE);

        // Load first batch using task-context helper logic (see below)
        size_t batch_start_idx = 0;
        size_t batch_end_idx = (BATCH_SIZE < n) ? BATCH_SIZE : n;
        size_t events_in_batch = batch_end_idx - batch_start_idx;

        ESP_LOGI(TAG, "Loading batch 1/%zu (%zu events)", g_phase_ctx.batches_total, events_in_batch);

        // Create timers for first batch (task context)
        for (size_t i = 0; i < events_in_batch; i++) {
            const TimelineEvent *ev = &g_phase_ctx.events[batch_start_idx + i];

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

            esp_timer_start_once(tmr, delay_us);
            g_phase_ctx.timers[i] = tmr;
        }

        // If there are more batches, set up batch timer for next load
        if (g_phase_ctx.batches_total > 1) {
            uint64_t last_event_time_us = g_phase_ctx.events[batch_end_idx - 1].fire_time_us;
            uint64_t delay_to_next_batch = last_event_time_us + 1000; // 1ms after last event

            const esp_timer_create_args_t batch_args = {
                .callback = load_next_batch_timer_cb,
                .arg = NULL,
                .name = "batch_load"
            };

            esp_err_t err = esp_timer_create(&batch_args, &g_phase_ctx.batch_timer);
            if (err == ESP_OK) {
                esp_timer_start_once(g_phase_ctx.batch_timer, delay_to_next_batch);
                ESP_LOGI(TAG, "Batch loader timer scheduled for %.3f seconds", delay_to_next_batch / 1000000.0);
            } else {
                ESP_LOGE(TAG, "Failed to create batch timer: %s", esp_err_to_name(err));
            }
        }

        ESP_LOGI(TAG, "Scheduled %zu events for phase %s in batches", n, phase->id);

        // If we have multiple batches, handle subsequent batches in task context
        while (g_phase_ctx.current_batch_idx + 1 < g_phase_ctx.batches_total && g_phase_ctx.active) {
            // Wait for the batch completion signal from timer callback
            if (g_phase_ctx.batch_sem) {
                xSemaphoreTake(g_phase_ctx.batch_sem, portMAX_DELAY);
            } else {
                // No semaphore: fall back to busy-waiting until batch_timer fires
                // (shouldn't happen normally)
                while (g_phase_ctx.current_batch_idx + 1 < g_phase_ctx.batches_total && g_phase_ctx.active) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                break;
            }

            // Delete previous batch timers (they should have fired)
            for (size_t i = 0; i < BATCH_SIZE; i++) {
                if (g_phase_ctx.timers[i]) {
                    esp_timer_stop(g_phase_ctx.timers[i]);
                    esp_timer_delete(g_phase_ctx.timers[i]);
                    g_phase_ctx.timers[i] = NULL;
                }
            }

            // stop and delete previous batch_timer if present
            if (g_phase_ctx.batch_timer) {
                esp_timer_stop(g_phase_ctx.batch_timer);
                esp_timer_delete(g_phase_ctx.batch_timer);
                g_phase_ctx.batch_timer = NULL;
            }

            // Move to next batch index and create timers for it (task context)
            g_phase_ctx.current_batch_idx++;
            size_t next_batch_start = g_phase_ctx.current_batch_idx * BATCH_SIZE;
            size_t next_batch_end = (next_batch_start + BATCH_SIZE < g_phase_ctx.num_events) ?
                                    next_batch_start + BATCH_SIZE : g_phase_ctx.num_events;
            size_t next_events = next_batch_end - next_batch_start;

            ESP_LOGI(TAG, "Loading batch %zu/%zu (%zu events)", 
                     g_phase_ctx.current_batch_idx + 1, g_phase_ctx.batches_total, next_events);

            uint64_t now_us2 = esp_timer_get_time();
            uint64_t batch_start_us2 = g_phase_ctx.batch_start_us;

            for (size_t i = 0; i < next_events; i++) {
                const TimelineEvent *ev = &g_phase_ctx.events[next_batch_start + i];

                const esp_timer_create_args_t args = {
                    .callback = event_timer_cb,
                    .arg = (void *)ev,
                    .name = "cycle_evt"
                };

                esp_timer_handle_t tmr;
                esp_err_t err = esp_timer_create(&args, &tmr);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "esp_timer_create failed for batch event: %s", esp_err_to_name(err));
                    continue;
                }

                uint64_t time_since_phase_start = now_us2 - batch_start_us2;
                uint64_t delay_us;
                if (ev->fire_time_us > time_since_phase_start) {
                    delay_us = ev->fire_time_us - time_since_phase_start;
                } else {
                    delay_us = 1000;
                }

                esp_timer_start_once(tmr, delay_us);
                g_phase_ctx.timers[i] = tmr;
            }

            // schedule next batch loader timer
            if (next_events > 0) {
                uint64_t last_event_time_us = g_phase_ctx.events[next_batch_end - 1].fire_time_us;
                uint64_t time_since_phase_start = now_us2 - batch_start_us2;
                uint64_t delay_to_next_batch;
                if (last_event_time_us > time_since_phase_start) {
                    delay_to_next_batch = (last_event_time_us - time_since_phase_start) + 1000;
                } else {
                    delay_to_next_batch = 1000;
                }

                const esp_timer_create_args_t batch_args = {
                    .callback = load_next_batch_timer_cb,
                    .arg = NULL,
                    .name = "batch_load"
                };

                esp_err_t err = esp_timer_create(&batch_args, &g_phase_ctx.batch_timer);
                if (err == ESP_OK) {
                    esp_timer_start_once(g_phase_ctx.batch_timer, delay_to_next_batch);
                    ESP_LOGI(TAG, "Batch timer set for %.3f seconds", delay_to_next_batch / 1000000.0);
                } else {
                    ESP_LOGE(TAG, "Failed to create batch timer: %s", esp_err_to_name(err));
                }
            }
        }
    }

    // ------------------------------------------------------------
    // Check if current phase's sensor trigger should fire
    // Returns true if threshold met and phase should skip
    // ------------------------------------------------------------
    static bool check_phase_sensor_trigger(void)
    {
        #define PHASE_SENSOR_COOLDOWN_MS 15000
        
        if (!cycle_running || current_phase_index <= 0 || current_phase_index > (int)g_num_phases) {
            return false;
        }

        // Get current phase (current_phase_index is 1-based)
        int phase_idx = current_phase_index - 1;
        if (phase_idx < 0 || phase_idx >= (int)g_num_phases) {
            return false;
        }

        Phase *phase = &g_phases[phase_idx];
        if (!phase->sensor_trigger) {
            return false;  // No trigger configured for this phase
        }

        SensorTrigger *trigger = phase->sensor_trigger;
        
        // Already triggered in this phase?
        if (trigger->has_triggered) {
            return false;
        }

        // COOLDOWN: Skip first 15 seconds of phase to avoid false triggers during transitions
        uint64_t now_us = esp_timer_get_time();
        uint64_t phase_elapsed_ms = (now_us >= phase_start_us) ? (now_us - phase_start_us) / 1000 : 0;
        if (phase_elapsed_ms < PHASE_SENSOR_COOLDOWN_MS) {
            return false;  // Still in cooldown period
        }

        // Read sensor value based on trigger type
        uint32_t sensor_value = 0;
        if (trigger->type == SENSOR_TYPE_RPM) {
            sensor_value = (uint32_t)rpm_sensor_get_rpm();
        } else if (trigger->type == SENSOR_TYPE_PRESSURE) {
            sensor_value = (uint32_t)pressure_sensor_read_frequency();
        } else {
            return false;  // Unknown sensor type
        }

        // Check if threshold condition met
        bool should_trigger = false;
        if (trigger->trigger_above) {
            should_trigger = (sensor_value > trigger->threshold);
        } else {
            should_trigger = (sensor_value < trigger->threshold);
        }

        if (should_trigger) {
            trigger->has_triggered = true;
            const char *sensor_name = (trigger->type == SENSOR_TYPE_RPM) ? "RPM" : "Pressure";
            ESP_LOGI(TAG, "Sensor trigger FIRED: %s=%u %s threshold=%u (phase elapsed: %llu ms)",
                     sensor_name, sensor_value,
                     trigger->trigger_above ? ">" : "<",
                     trigger->threshold, phase_elapsed_ms);
        }

        return should_trigger;
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

        // stop all pending timers in current batch
        for (size_t i = 0; i < BATCH_SIZE; i++) {
            if (g_phase_ctx.timers[i]) {
                esp_timer_stop(g_phase_ctx.timers[i]);
                esp_timer_delete(g_phase_ctx.timers[i]);
                g_phase_ctx.timers[i] = NULL;
            }
        }

        // stop batch loader timer if it exists
        if (g_phase_ctx.batch_timer) {
            esp_timer_stop(g_phase_ctx.batch_timer);
            esp_timer_delete(g_phase_ctx.batch_timer);
            g_phase_ctx.batch_timer = NULL;
        }

        // Delete batch semaphore if present
        if (g_phase_ctx.batch_sem) {
            vSemaphoreDelete(g_phase_ctx.batch_sem);
            g_phase_ctx.batch_sem = NULL;
        }

        if (force_off_all) {
            // turn OFF everything (active-low → 1)
            for (int i = 0; i < NUM_COMPONENTS; i++) {
                gpio_set_level(all_pins[i], 1);
                gpio_shadow[i] = 1;
            }
        }

        g_phase_ctx.active = false;
        // NOTE: do NOT set cycle_running = false here; let run_cycle() control it
        ESP_LOGW(TAG, "Current phase skipped/cancelled.");
    }

    void cycle_skip_to_phase(size_t phase_index)
    {
        if (!cycle_running) {
            ESP_LOGW(TAG, "cycle_skip_to_phase: no cycle running");
            return;
        }

        // Set target phase and skip current
        target_phase_index = (int)phase_index;
        cycle_skip_current_phase(true);
        ESP_LOGI(TAG, "Skipping to phase %zu", phase_index);
    }

    void cycle_stop(void)
    {
        if (!cycle_running) {
            ESP_LOGW(TAG, "cycle_stop: no cycle running");
            return;
        }

        // Set special stop flag
        target_phase_index = -2;
        cycle_skip_current_phase(true);
        ESP_LOGI(TAG, "Cycle stop requested");
    }


    void run_cycle(Phase *phases, size_t num_phases)
    {
        cycle_running = true;
        target_phase_index = -1;
        
        size_t heap_at_start = esp_get_free_heap_size();
        ESP_LOGI(TAG, "=== CYCLE START: Free heap = %zu bytes ===", heap_at_start);

        for (size_t i = 0; i < num_phases; i++) {
            // Check if we should stop the entire cycle
            if (target_phase_index == -2) {
                ESP_LOGW(TAG, "Cycle stop signal detected, breaking out of cycle loop");
                target_phase_index = -1;
                break;
            }

            // Check if we should skip to a different phase
            if (target_phase_index >= 0) {
                if (target_phase_index >= (int)num_phases) {
                    ESP_LOGW(TAG, "skip_to_phase index out of bounds (%d >= %zu)", target_phase_index, num_phases);
                    target_phase_index = -1;
                    break;
                }
                i = target_phase_index - 1;  // -1 because loop will increment
                target_phase_index = -1;
                continue;
            }
            
            // Log heap before each phase
            size_t heap_before_phase = esp_get_free_heap_size();
            ESP_LOGI(TAG, "Phase %zu start - Free heap: %zu bytes (delta: %ld)", 
                     i+1, heap_before_phase, (long)heap_before_phase - (long)heap_at_start);

            current_phase_index = (int)i + 1;  // update current phase index for telemetry
            Phase *p = &phases[i];

            ESP_LOGI(TAG, "=== Running phase %d: %s ===", (int)i + 1, p->id);
            run_phase_with_esp_timer(p);

            // Wait for phase to complete, checking sensor triggers and yielding to other tasks
            while (g_phase_ctx.active) {
                // Check if sensor trigger should skip this phase
                if (check_phase_sensor_trigger()) {
                    cycle_skip_current_phase(true);
                    break;  // Exit wait loop, phase will be skipped
                }
                
                vTaskDelay(pdMS_TO_TICKS(100));  // Check sensor every 100ms
                // Explicitly yield to let higher priority tasks run (e.g., WebSocket)
                taskYIELD();
            }

            // Ensure all timers from this phase are cleaned up before moving to next
            // Clean up current batch timers
            for (size_t j = 0; j < BATCH_SIZE; j++) {
                if (g_phase_ctx.timers[j]) {
                    esp_timer_stop(g_phase_ctx.timers[j]);
                    esp_timer_delete(g_phase_ctx.timers[j]);
                    g_phase_ctx.timers[j] = NULL;
                }
            }
            
            // Clean up batch loader timer
            if (g_phase_ctx.batch_timer) {
                esp_timer_stop(g_phase_ctx.batch_timer);
                esp_timer_delete(g_phase_ctx.batch_timer);
                g_phase_ctx.batch_timer = NULL;
            }
        
            // Delete batch semaphore if present
            if (g_phase_ctx.batch_sem) {
                vSemaphoreDelete(g_phase_ctx.batch_sem);
                g_phase_ctx.batch_sem = NULL;
            }
            
            // Small delay to allow other tasks to run (especially WebSocket)
            vTaskDelay(pdMS_TO_TICKS(10));
            taskYIELD();  // Ensure other tasks get a chance
        }

        size_t heap_at_end = esp_get_free_heap_size();
        ESP_LOGI(TAG, "=== CYCLE COMPLETED - Free heap: %zu bytes (delta: %ld) ===", 
                 heap_at_end, (long)heap_at_end - (long)heap_at_start);

        cycle_running = false;
        current_phase_index = 0;
    }

    // Free memory from previously loaded cycle
    void cycle_unload(void)
    {
        ESP_LOGI(TAG, "Unloading previous cycle...");

        // Free the JSON tree if we have one
        if (g_loaded_cycle_json) {
            cJSON_Delete(g_loaded_cycle_json);
            g_loaded_cycle_json = NULL;
            ESP_LOGI(TAG, "Previous cycle JSON freed");
        }

        // Reset pools and counts
        g_motor_cfg_used = 0;
        g_motor_steps_used = 0;
        g_sensor_trigger_used = 0;
        g_num_phases = 0;
        
        // Clear static arrays
        memset(g_phases, 0, sizeof(g_phases));
        memset(g_components_pool, 0, sizeof(g_components_pool));
        memset(g_sensor_trigger_pool, 0, sizeof(g_sensor_trigger_pool));
        memset(g_motor_cfg_pool, 0, sizeof(g_motor_cfg_pool));
        memset(g_motor_steps_pool, 0, sizeof(g_motor_steps_pool));

        ESP_LOGI(TAG, "Cycle unloaded, memory freed");
    }

    esp_err_t cycle_load_from_json_str(const char *json_str)
    {
        if (!json_str) {
            ESP_LOGE(TAG, "cycle_load_from_json_str: null json_str");
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "Starting cycle load from JSON...");

        // Free any previously loaded cycle
        cycle_unload();

        ESP_LOGI(TAG, "Pools reset. MAX_MOTOR_STEPS=%d, MAX_PHASES=%d", MAX_MOTOR_STEPS, MAX_PHASES);

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
            ESP_LOGI(TAG, "Loaded %zu phases into RAM. Motor configs used: %zu, Motor steps used: %zu/%d", 
                    g_num_phases, g_motor_cfg_used, g_motor_steps_used, MAX_MOTOR_STEPS);
            // Update WebSocket cycle_data cache with the newly loaded cycle
            ws_update_cycle_data_cache();
        } else {
            ESP_LOGE(TAG, "Failed to load cycle from JSON. Motor configs used: %zu, Motor steps used: %zu/%d", 
                    g_motor_cfg_used, g_motor_steps_used, MAX_MOTOR_STEPS);
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
        
        // Reset sensors before starting new cycle for clean data
        ESP_LOGI(TAG, "Resetting sensors before starting cycle...");
        rpm_sensor_reset();
        pressure_sensor_reset();
        ESP_LOGI(TAG, "Sensors reset complete");
        
        // Create a background task to run the cycle so WebSocket stays responsive
        // Use priority 2 to avoid starving WebSocket task (which runs at ~1-2)
        xTaskCreatePinnedToCore(
            cycle_task,           // task function
            "cycle_runner",       // task name
            4096,                 // stack size
            NULL,                 // parameter
            2,                    // priority (same as WebSocket to allow fair scheduling)
            NULL,                 // task handle (not needed for cleanup)
            0                     // core 0
        );
    }