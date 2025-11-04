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

// ------------------------- DATA STRUCTS -------------------------
typedef struct {
    const char *id;
    const char *label;
    const char *compId;      // e.g. "Retractor", "Cold Valve"
    uint32_t    start_ms;
    uint32_t    duration_ms;
    bool        has_motor;
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
    
} TimelineEvent;

// ------------------------- API -------------------------
void init_all_gpio(void);
size_t build_timeline_from_phase(const Phase *phase,
                                 TimelineEvent *out_events,
                                 size_t max_events);
void run_phase_with_esp_timer(const Phase *phase);

void start_gpio_monitor(void);
void stop_gpio_monitor(void);



void run_cycle(Phase *phases, size_t num_phases);