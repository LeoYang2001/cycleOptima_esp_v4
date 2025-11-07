// telemetry.h
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

// ====================== DATA STRUCTURES ======================

// GPIO states snapshot
typedef struct {
    uint8_t pin_number;
    uint8_t state;  // 0 or 1
} GpioState;

#define MAX_GPIO_PINS 8

typedef struct {
    GpioState pins[MAX_GPIO_PINS];
    uint8_t num_pins;
    uint64_t timestamp_ms;
} GpioTelemetry;

// Sensor data (expandable for future sensors)
typedef struct {
    float temperature;      // e.g., from temp sensor
    float pressure;         // e.g., from pressure sensor
    float flow_rate;        // e.g., from flow sensor
    bool sensor_error;
    uint64_t timestamp_ms;
} SensorTelemetry;

// Current cycle and phase information
typedef struct {
    bool cycle_running;
    uint32_t current_phase_index;
    const char *current_phase_name;
    uint32_t total_phases;
    uint32_t phase_elapsed_ms;
    uint32_t phase_total_duration_ms;
    uint64_t cycle_start_time_ms;
    uint64_t timestamp_ms;
} CycleTelemetry;

// Unified telemetry packet (all data in one snapshot)
typedef struct {
    GpioTelemetry gpio;
    SensorTelemetry sensors;
    CycleTelemetry cycle;
    uint64_t packet_timestamp_ms;
} TelemetryPacket;

// ====================== API ======================

/**
 * Initialize the telemetry system (call once at startup)
 * Starts the background monitoring task
 * @param update_interval_ms: how often to gather data (e.g., 100 ms)
 */
void telemetry_init(uint32_t update_interval_ms);

/**
 * Stop the telemetry monitoring task
 */
void telemetry_stop(void);

/**
 * Get the latest telemetry snapshot
 * Safe to call from any task
 * @return latest TelemetryPacket
 */
TelemetryPacket telemetry_get_latest(void);

/**
 * Register a callback to be invoked whenever new telemetry is gathered
 * Callback is called from the telemetry task, so keep it fast
 * Set callback to NULL to disable
 */
typedef void (*telemetry_callback_t)(const TelemetryPacket *packet);
void telemetry_set_callback(telemetry_callback_t callback);

/**
 * For manual sensor updates (e.g., from an ADC reading task)
 * Thread-safe
 */
void telemetry_update_sensor(const SensorTelemetry *sensor_data);

/**
 * For manual cycle info updates (e.g., from cycle.c)
 * Thread-safe
 */
void telemetry_update_cycle(const CycleTelemetry *cycle_data);
