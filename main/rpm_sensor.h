// rpm_sensor.h
#pragma once

#include <stdbool.h>
#include <stdint.h>

// initialize the RPM sensor on pin 0 (hardcoded per your note)
void rpm_sensor_init(void);

/**
 * Get latest RPM (normalized).
 * - returns RPM as float
 * - returns 0.0f if timed out or not enough data
 */
float rpm_sensor_get_rpm(void);

/**
 * Optionally set pulses per revolution.
 * Default = 1.0f
 */
void rpm_sensor_set_pulses_per_rev(float ppr);

/**
 * Reset internal state (clear timestamps, rpm)
 */
void rpm_sensor_reset(void);
