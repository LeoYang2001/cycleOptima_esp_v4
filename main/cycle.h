// cycle.h
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/gpio.h"

// ------------------------- PIN MAPPINGS -------------------------
#define RETRACTOR_PIN        GPIO_NUM_7
#define DETERGENT_VALVE_PIN  GPIO_NUM_8
#define COLD_VALVE_PIN       GPIO_NUM_5
#define DRAIN_PUMP_PIN       GPIO_NUM_19
#define HOT_VALVE_PIN        GPIO_NUM_9
#define SOFT_VALVE_PIN       GPIO_NUM_18
#define MOTOR_ON_PIN         GPIO_NUM_4
#define MOTOR_DIRECTION_PIN  GPIO_NUM_10
#define FLOW_SENSOR_PIN      GPIO_NUM_0
#define NUM_COMPONENTS       8

// -------------------- MOTOR TYPES --------------------
// one entry in "pattern": { stepTime, pauseTime, direction }
typedef struct {
    uint32_t step_time_ms;    // "stepTime"
    uint32_t pause_time_ms;   // "pauseTime"
    const char *direction;    // "cw" or "ccw"
} MotorPatternStep;

// the full motorConfig block
typedef struct {
    int repeat_times;               // "repeatTimes"
    MotorPatternStep *pattern;      // array of steps
    size_t pattern_len;             // number of steps
    const char *running_style;      // optional, e.g. "Single Direction"
} MotorConfig;

// -------------------- PHASE TYPES --------------------
typedef struct {
    const char *id;
    const char *label;
    const char *compId;
    uint32_t    start_ms;
    uint32_t    duration_ms;
    bool        has_motor;          // false for normal components
    MotorConfig *motor_cfg;         // NULL for normal components
} PhaseComponent;

typedef struct {
    const char     *id;
    const char     *name;
    const char     *color;
    uint32_t        start_time_ms;
    PhaseComponent *components;
    size_t          num_components;
} Phase;

typedef enum {
    EVENT_ON,
    EVENT_OFF
} EventType;

typedef struct {
    uint64_t    fire_time_us;
    EventType   type;
    gpio_num_t  pin;
        int         level;     // for motor direction
} TimelineEvent;



esp_err_t load_cycle_from_json_str(const char *json_str,
                                   Phase *phases,
                                   size_t max_phases,
                                   PhaseComponent *components_pool,
                                   size_t max_components_per_phase,
                                   size_t *out_num_phases);



 // STATE MACHINE FUNCTIONS
esp_err_t cycle_load_from_json_str(const char *json_str);
void cycle_run_loaded_cycle(void);
void init_all_gpio(void);
void cycle_skip_current_phase(bool force_off_all);
void cycle_skip_to_phase(size_t phase_index);
bool cycle_is_running(void);


// ------------------------- API -------------------------
void start_gpio_monitor(void);
void stop_gpio_monitor(void);
void run_phase_with_esp_timer(const Phase *phase);
void run_cycle(Phase *phases, size_t num_phases);



