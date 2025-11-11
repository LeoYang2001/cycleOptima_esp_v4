// rpm_sensor.c

#include "rpm_sensor.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"  // for critical sections
#include "esp_attr.h"

#define RPM_SENSOR_PIN          0
#define RPM_DEBOUNCE_US         2000        // 2 ms debounce, adjust as needed
#define RPM_TIMEOUT_MS          2000        // if no pulse for 2s -> rpm = 0
#define RPM_TS_COUNT            3           // 3-sample smoothing

// shared state between ISR and readers
static volatile uint64_t s_timestamps[RPM_TS_COUNT] = {0};
static volatile int      s_ts_index = 0;
static volatile uint64_t s_last_pulse_us = 0;

// normalization
static volatile float s_pulses_per_rev = 1.0f;

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
    vPortExitCritical();
}

/**
 * Compute RPM from the last 3 timestamps.
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

    // timeout: no pulse for RPM_TIMEOUT_MS
    if ((now_us - ts_local[idx_local]) > (RPM_TIMEOUT_MS * 1000ULL)) {
        return 0.0f;
    }

    // need 3 valid timestamps
    int i1 = idx_local;
    int i2 = (i1 - 1 + RPM_TS_COUNT) % RPM_TS_COUNT;
    int i3 = (i2 - 1 + RPM_TS_COUNT) % RPM_TS_COUNT;

    if (ts_local[i1] == 0 || ts_local[i2] == 0 || ts_local[i3] == 0) {
        // not enough data yet
        return 0.0f;
    }

    uint64_t d1 = ts_local[i1] - ts_local[i2];
    uint64_t d2 = ts_local[i2] - ts_local[i3];

    if (d1 == 0 || d2 == 0) {
        return 0.0f;
    }

    // pulses/sec from both intervals
    float f1 = 1e6f / (float)d1;
    float f2 = 1e6f / (float)d2;
    float freq_hz = (f1 + f2) * 0.5f;   // smooth a bit

    // normalize to RPM: freq_hz / (pulses/rev) * 60
    float rpm = (freq_hz / ppr_local) * 60.0f;

    // optional clamp
    if (rpm < 0.0f) rpm = 0.0f;

    return rpm;
}
