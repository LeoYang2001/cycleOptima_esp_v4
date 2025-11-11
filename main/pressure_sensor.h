// pressure_sensor.h
#ifndef PRESSURE_SENSOR_H
#define PRESSURE_SENSOR_H

#include <stdint.h>
#include <stdbool.h>

// initialize GPIO & calibration defaults
void pressure_sensor_init(void);

// read one filtered/averaged pressure in kPa (legacy, kept for compatibility)
float pressure_sensor_read_kpa(void);

// read raw frequency (Hz) converted from raw 24-bit sensor value
// Uses formula: Freq = 28116.48 - 0.0014180 × Raw - 7 × 10^-11 × Raw²
float pressure_sensor_read_frequency(void);

// optional: get raw 24-bit value (if monitor wants to log it)
long pressure_sensor_read_raw(void);

// reset calibration / zero
void pressure_sensor_reset(void);

#endif // PRESSURE_SENSOR_H
