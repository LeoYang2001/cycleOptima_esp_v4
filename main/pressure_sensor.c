// pressure_sensor.c
#include "pressure_sensor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"   // for esp_rom_delay_us

static const char *TAG = "pressureSensor";

// pins (you can move to main.h if you like)
#define PRESS_DOUT_PIN  3   // HX710/HX711 DOUT
#define PRESS_SCK_PIN   2   // HX710/HX711 SCK

// calibration / conversion
// these are basically what you had
static volatile long  s_raw_zero = 0;
static volatile float s_kpa_to_cmh2o = 10.197f;   // conversion factor: 1 kPa = 10.197 cmH2O

// averaging settings
#define PRESS_AVG_SAMPLES     10
#define PRESS_CAPTURE_SAMPLES 20

// ------------------------------------------------------------------
// low-level helpers
// ------------------------------------------------------------------
static inline void wait_ready(void)
{
    // HX711 pulls DOUT low when data is ready
    while (gpio_get_level(PRESS_DOUT_PIN) == 1) {
        // short wait; we are in a called context, not a task loop
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static long read_raw_once(void)
{
    unsigned long value = 0;

    wait_ready();

    // same read sequence as before
    vPortEnterCritical();
    for (int i = 0; i < 24; i++) {
        gpio_set_level(PRESS_SCK_PIN, 1);
        esp_rom_delay_us(1);
        value = (value << 1) | (gpio_get_level(PRESS_DOUT_PIN) ? 1UL : 0UL);
        gpio_set_level(PRESS_SCK_PIN, 0);
        esp_rom_delay_us(1);
    }
    // 25th pulse for gain
    gpio_set_level(PRESS_SCK_PIN, 1);
    esp_rom_delay_us(1);
    gpio_set_level(PRESS_SCK_PIN, 0);
    vPortExitCritical();

    // sign-extend 24-bit
    if (value & 0x800000UL) {
        value |= 0xFF000000UL;
    }
    return (long)value;
}

static long read_raw_averaged(int n)
{
    long long sum = 0;
    for (int i = 0; i < n; i++) {
        sum += read_raw_once();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return (long)(sum / n);
}

static float raw_to_kpa(long raw)
{
    // Not used anymore - we calculate frequency directly from raw
    (void)raw;
    return 0.0f;
}

// Convert raw sensor data to frequency (Hz) using quadratic formula
// Freq = 28116.48 - 0.0014180 × Raw - 7 × 10^-11 × Raw²
static float raw_to_frequency(long raw)
{
    float freq = 28116.48f - (0.0014180f * raw) - (7e-11f * raw * raw);
    return freq;
}

// ------------------------------------------------------------------
// public API
// ------------------------------------------------------------------
void pressure_sensor_init(void)
{
    // SCK output
    gpio_config_t io_conf_sck = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << PRESS_SCK_PIN),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    gpio_config(&io_conf_sck);

    // DOUT input
    gpio_config_t io_conf_dout = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << PRESS_DOUT_PIN),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    gpio_config(&io_conf_dout);

    gpio_set_level(PRESS_SCK_PIN, 0);

    // take an initial zero capture
    s_raw_zero = read_raw_averaged(PRESS_CAPTURE_SAMPLES);
    ESP_LOGI(TAG, "pressure init: zero=%ld", s_raw_zero);
}

float pressure_sensor_read_kpa(void)
{
    long raw = read_raw_averaged(PRESS_AVG_SAMPLES);
    float kpa = raw_to_kpa(raw);
    return kpa;
}

long pressure_sensor_read_raw(void)
{
    return read_raw_once();
}

float pressure_sensor_read_frequency(void)
{
    long raw = read_raw_averaged(PRESS_AVG_SAMPLES);
    float freq = raw_to_frequency(raw);
    return freq;
}

void pressure_sensor_reset(void)
{
    s_raw_zero = read_raw_averaged(PRESS_CAPTURE_SAMPLES);
    ESP_LOGI(TAG, "pressure reset: zero=%ld", s_raw_zero);
}
