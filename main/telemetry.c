// telemetry.c
#include "telemetry.h"
#include "cycle.h"
#include "rpm_sensor.h"
#include "pressure_sensor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "telemetry";

// ====================== GLOBAL STATE ======================
static TaskHandle_t telemetry_task_handle = NULL;
static SemaphoreHandle_t telemetry_mutex = NULL;
static TelemetryPacket g_telemetry_latest = {0};
static telemetry_callback_t g_telemetry_callback = NULL;
static uint32_t g_update_interval_ms = 100;
static bool g_telemetry_running = false;

// GPIO pin list (from cycle.h)
extern const gpio_num_t all_pins[NUM_COMPONENTS];
extern int gpio_shadow[NUM_COMPONENTS];

// Cycle state variables (from cycle.c)
extern uint64_t phase_start_us;
extern bool cycle_running;
extern const char *current_phase_name;
extern int current_phase_index;
extern size_t g_num_phases;

// ====================== INTERNAL HELPERS ======================

/**
 * Gather GPIO states
 */
static void gather_gpio_telemetry(GpioTelemetry *gpio_tel)
{
    gpio_tel->num_pins = NUM_COMPONENTS;
    gpio_tel->timestamp_ms = esp_timer_get_time() / 1000;

    for (int i = 0; i < NUM_COMPONENTS && i < MAX_GPIO_PINS; i++) {
        gpio_tel->pins[i].pin_number = all_pins[i];
        gpio_tel->pins[i].state = gpio_shadow[i];
    }
}

/**
 * Gather sensor data (RPM from rpm_sensor, pressure frequency from pressure_sensor)
 */
static void gather_sensor_telemetry(SensorTelemetry *sensor_tel)
{
    // Read RPM from the rpm_sensor module
    sensor_tel->rpm = rpm_sensor_get_rpm();
    
    // Read pressure frequency (Hz) from the pressure_sensor module
    sensor_tel->pressure_freq = pressure_sensor_read_frequency();
    
    sensor_tel->sensor_error = false;
    sensor_tel->timestamp_ms = esp_timer_get_time() / 1000;
}

/**
 * Gather cycle and phase information
 */
static void gather_cycle_telemetry(CycleTelemetry *cycle_tel)
{
    cycle_tel->cycle_running = cycle_running;
    cycle_tel->current_phase_index = current_phase_index;
    cycle_tel->current_phase_name = (char *)current_phase_name;
    cycle_tel->total_phases = g_num_phases;
    
    // Calculate phase elapsed time (relative to phase_start_us)
    uint64_t now_us = esp_timer_get_time();
    uint64_t elapsed_us = (now_us >= phase_start_us) ? (now_us - phase_start_us) : 0;
    cycle_tel->phase_elapsed_ms = elapsed_us / 1000;
    
    // TODO: phase_total_duration_ms, cycle_start_time_ms (need phase duration info from cycle.c)
    cycle_tel->phase_total_duration_ms = 0;
    cycle_tel->cycle_start_time_ms = 0;
    
    // Use phase-relative time for timestamp (0 = start of phase)
    cycle_tel->timestamp_ms = elapsed_us / 1000;
}

// ====================== BACKGROUND TASK ======================

/**
 * The telemetry gathering task runs at regular intervals
 * Collects GPIO, sensor, and cycle data into a single packet
 */
static void telemetry_task(void *pvParameter)
{
    ESP_LOGI(TAG, "Telemetry task started (update interval: %u ms)", g_update_interval_ms);

    while (g_telemetry_running) {
        // Gather all telemetry
        TelemetryPacket packet = {0};
        packet.packet_timestamp_ms = esp_timer_get_time() / 1000;

        gather_gpio_telemetry(&packet.gpio);
        gather_sensor_telemetry(&packet.sensors);
        gather_cycle_telemetry(&packet.cycle);

        // Update the latest snapshot (thread-safe)
        if (xSemaphoreTake(telemetry_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            g_telemetry_latest = packet;
            xSemaphoreGive(telemetry_mutex);
        }

        // Call user callback if registered
        if (g_telemetry_callback) {
            g_telemetry_callback(&packet);
        }

        // Log to console only when cycle is running (GPIO states, cycle info, RPM, pressure frequency)
        if (packet.cycle.cycle_running) {
            printf("[%lu ms] GPIO: ", packet.cycle.phase_elapsed_ms);
            for (int i = 0; i < packet.gpio.num_pins; i++) {
                printf("%d:%d ", packet.gpio.pins[i].pin_number, packet.gpio.pins[i].state);
            }
            printf(" | Cycle: %s (Phase: %ld/%lu) | RPM: %.0f | PressFreq: %.2f Hz\n",
                   packet.cycle.cycle_running ? "RUNNING" : "IDLE",
                   (long)packet.cycle.current_phase_index + 1,
                   (unsigned long)packet.cycle.total_phases,
                   packet.sensors.rpm,
                   packet.sensors.pressure_freq);
        }

        // Wait for next update interval
        vTaskDelay(pdMS_TO_TICKS(g_update_interval_ms));
    }

    ESP_LOGI(TAG, "Telemetry task stopped");
    vTaskDelete(NULL);
}

// ====================== PUBLIC API ======================

void telemetry_init(uint32_t update_interval_ms)
{
    if (telemetry_task_handle != NULL) {
        ESP_LOGW(TAG, "telemetry_init: already initialized");
        return;
    }

    g_update_interval_ms = update_interval_ms;
    g_telemetry_running = true;

    // Create mutex for thread-safe access to g_telemetry_latest
    telemetry_mutex = xSemaphoreCreateMutex();
    if (!telemetry_mutex) {
        ESP_LOGE(TAG, "Failed to create telemetry mutex");
        return;
    }

    // Create the background telemetry task
    xTaskCreatePinnedToCore(
        telemetry_task,
        "telemetry",
        4096,
        NULL,
        4,  // priority
        &telemetry_task_handle,
        0   // core 0
    );

    ESP_LOGI(TAG, "Telemetry system initialized (interval: %u ms)", update_interval_ms);
}

void telemetry_stop(void)
{
    if (telemetry_task_handle == NULL) {
        return;
    }

    g_telemetry_running = false;
    vTaskDelay(pdMS_TO_TICKS(200));  // give task time to exit
    telemetry_task_handle = NULL;

    if (telemetry_mutex) {
        vSemaphoreDelete(telemetry_mutex);
        telemetry_mutex = NULL;
    }

    ESP_LOGI(TAG, "Telemetry system stopped");
}

TelemetryPacket telemetry_get_latest(void)
{
    TelemetryPacket result = {0};

    if (xSemaphoreTake(telemetry_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        result = g_telemetry_latest;
        xSemaphoreGive(telemetry_mutex);
    } else {
        ESP_LOGW(TAG, "Failed to acquire telemetry mutex");
    }

    return result;
}

void telemetry_set_callback(telemetry_callback_t callback)
{
    g_telemetry_callback = callback;
    ESP_LOGI(TAG, "Telemetry callback %s", callback ? "registered" : "unregistered");
}

void telemetry_update_sensor(const SensorTelemetry *sensor_data)
{
    if (!sensor_data || !telemetry_mutex) return;

    if (xSemaphoreTake(telemetry_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_telemetry_latest.sensors = *sensor_data;
        xSemaphoreGive(telemetry_mutex);
    }
}

void telemetry_update_cycle(const CycleTelemetry *cycle_data)
{
    if (!cycle_data || !telemetry_mutex) return;

    if (xSemaphoreTake(telemetry_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_telemetry_latest.cycle = *cycle_data;
        xSemaphoreGive(telemetry_mutex);
    }
}


