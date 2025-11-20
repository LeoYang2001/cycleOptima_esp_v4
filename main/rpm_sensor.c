// rpm_sensor.c

#include "rpm_sensor.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"  // for critical sections
#include "esp_attr.h"
#include <math.h>  // for sqrtf and fabsf

#define RPM_SENSOR_PIN          0
#define RPM_DEBOUNCE_US         2000        // 2 ms debounce, adjust as needed
#define RPM_TIMEOUT_MS          2000        // if no pulse for 2s -> rpm = 0
#define RPM_TS_COUNT            3           // 3-sample for simpler, more reliable calculation
#define RPM_MAX_LIMIT           1500.0f     // Maximum realistic RPM - ignore readings above this

// shared state between ISR and readers
static volatile uint64_t s_timestamps[RPM_TS_COUNT] = {0};
static volatile int      s_ts_index = 0;
static volatile uint64_t s_last_pulse_us = 0;

// normalization
static volatile float s_pulses_per_rev = 1.0f;

// acceleration limiting - track last valid RPM reading
static float s_last_avg_rpm = 0.0f;

// ISR: capture pulses and store timestamps
static void IRAM_ATTR rpm_gpio_isr(void *arg)
{
    uint64_t now = esp_timer_get_time();

    // debounce
    if ((now - s_last_pulse_us) < RPM_DEBOUNCE_US) {
        return;
    }
    s_last_pulse_us = now;

    // rotate circular buffer
    int next_idx = (s_ts_index + 1) % RPM_TS_COUNT;
    s_timestamps[next_idx] = now;
    s_ts_index = next_idx;
}

void rpm_sensor_init(void)
{
    // configure GPIO 0 as input with rising-edge interrupt
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_POSEDGE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << RPM_SENSOR_PIN),
        .pull_up_en = 1,
    };
    gpio_config(&io_conf);

    // install ISR service (if not already done elsewhere)
    gpio_install_isr_service(0);
    gpio_isr_handler_add(RPM_SENSOR_PIN, rpm_gpio_isr, NULL);
}

void rpm_sensor_set_pulses_per_rev(float ppr)
{
    if (ppr > 0.0f) {
        s_pulses_per_rev = ppr;
    }
}

void rpm_sensor_reset(void)
{
    vPortEnterCritical();
    for (int i = 0; i < RPM_TS_COUNT; i++) {
        s_timestamps[i] = 0;
    }
    s_ts_index = 0;
    s_last_pulse_us = 0;
    s_last_avg_rpm = 0.0f;  // Reset last RPM tracking
    vPortExitCritical();
}

/**
 * Check if the new RPM reading is within acceptable acceleration limits
 * compared to the last reading to prevent unrealistic jumps
 */
static float apply_acceleration_limit(float new_rpm, float last_rpm)
{
    // If this is the first reading or last reading was 0, accept the new reading
    if (last_rpm == 0.0f) {
        return new_rpm;
    }
    
    // Calculate the difference between new and old RPM
    float rpm_diff = fabsf(new_rpm - last_rpm);
    
    // Determine maximum allowed change based on current RPM range
    float max_change;
    if (last_rpm < 250.0f) {
        max_change = 100.0f;  // Low RPM: allow up to 100 RPM change
    } else if (last_rpm <= 600.0f) {
        max_change = 50.0f;   // Medium RPM: allow up to 50 RPM change
    } else {
        max_change = 30.0f;   // High RPM: allow up to 30 RPM change
    }
    
    // If change is within limits, accept the new reading
    if (rpm_diff <= max_change) {
        return new_rpm;
    }
    
    // If change is too large, limit it to the maximum allowed change
    if (new_rpm > last_rpm) {
        // RPM is increasing - limit to last_rpm + max_change
        return last_rpm + max_change;
    } else {
        // RPM is decreasing - limit to last_rpm - max_change
        return last_rpm - max_change;
    }
}

/**
 * Compute RPM from the last 3 timestamps with bounds checking.
 * Based on the reference flow sensor code for more reliable calculation.
 * We do the math here (not in ISR) to keep ISR cheap.
 */
float rpm_sensor_get_rpm(void)
{
    uint64_t ts_local[RPM_TS_COUNT];
    int idx_local;
    float ppr_local;

    // copy shared state under a short critical section
    vPortEnterCritical();
    for (int i = 0; i < RPM_TS_COUNT; i++) {
        ts_local[i] = s_timestamps[i];
    }
    idx_local = s_ts_index;
    ppr_local = s_pulses_per_rev;
    vPortExitCritical();

    if (ppr_local <= 0.0f) {
        ppr_local = 1.0f;
    }

    uint64_t now_us = esp_timer_get_time();

    // timeout: no pulse for RPM_TIMEOUT_MS -> return 0
    // Motor may be OFF, stalled, or coasting without pulses
    if ((now_us - ts_local[idx_local]) > (RPM_TIMEOUT_MS * 1000ULL)) {
        return 0.0f;
    }

    // need 3 valid timestamps (like the reference code)
    int idx_1 = idx_local;
    int idx_2 = (idx_1 - 1 + RPM_TS_COUNT) % RPM_TS_COUNT;
    int idx_3 = (idx_2 - 1 + RPM_TS_COUNT) % RPM_TS_COUNT;

    // Check if we have valid timestamps
    if (ts_local[idx_1] == 0 || ts_local[idx_2] == 0 || ts_local[idx_3] == 0) {
        return 0.0f;
    }

    // Calculate two time differences between three consecutive timestamps (like reference)
    uint64_t time_diff_1 = ts_local[idx_1] - ts_local[idx_2];
    uint64_t time_diff_2 = ts_local[idx_2] - ts_local[idx_3];

    // Only calculate if we have valid time differences
    if (time_diff_1 == 0 || time_diff_2 == 0) {
        return 0.0f;
    }

    // Calculate RPM from both time differences (like reference code)
    float rpm_1 = (60.0f * 1000000.0f) / (float)time_diff_1;  // Convert microseconds to RPM
    float rpm_2 = (60.0f * 1000000.0f) / (float)time_diff_2;

    // Apply pulses per revolution normalization
    rpm_1 = rpm_1 / ppr_local;
    rpm_2 = rpm_2 / ppr_local;

    // Apply reasonable limits to individual calculations before averaging
    if (rpm_1 > RPM_MAX_LIMIT) rpm_1 = 0.0f;  // Ignore impossible readings
    if (rpm_2 > RPM_MAX_LIMIT) rpm_2 = 0.0f;

    // Average the valid RPM calculations
    float rpm = 0.0f;
    int valid_readings = 0;
    
    if (rpm_1 > 0.0f) {
        rpm += rpm_1;
        valid_readings++;
    }
    if (rpm_2 > 0.0f) {
        rpm += rpm_2;
        valid_readings++;
    }
    
    if (valid_readings > 0) {
        rpm = rpm / valid_readings;
    }

    // Final bounds check
    if (rpm > RPM_MAX_LIMIT) rpm = 0.0f;  // Cap at max RPM
    if (rpm < 0.0f) rpm = 0.0f;

    // Apply acceleration limiting to prevent unrealistic jumps
    // BUT: skip limiter if transitioning from 0 (motor just turned on)
    float limited_rpm = rpm;
    if (s_last_avg_rpm > 0.0f && rpm > 0.0f) {
        // Both previous and current are non-zero: apply limiter to smooth noise
        limited_rpm = apply_acceleration_limit(rpm, s_last_avg_rpm);
    }
    // If last was 0 and now non-zero, or last non-zero and now 0, use raw value
    
    // Update the last RPM reading for next comparison
    s_last_avg_rpm = limited_rpm;

    return limited_rpm;
}
