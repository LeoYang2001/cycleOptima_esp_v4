#include "ws_cycle.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_https_server.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "fs.h"           // fs_write_file(...)
#include "cycle.h"        // cycle_load_from_json_str(...), cycle_run_loaded_cycle(...)
#include "telemetry.h"    // TelemetryPacket, telemetry_set_callback()

static const char *TAG = "ws_cycle";

static httpd_handle_t s_server = NULL;

static uint16_t s_server_port = 0;  // Store port for external logging

// Structure for passing JSON data to processing task
typedef struct {
    char *json_data;
    size_t json_length;
    httpd_req_t *req;
    bool success;
} json_task_params_t;

uint16_t ws_cycle_get_port(void)
{
    return s_server_port;
}

// Broadcast a message to all connected WebSocket clients
void ws_broadcast_text(const char *msg)
{
    if (!s_server) {
        return;  // Server not started yet
    }

    httpd_ws_frame_t ws_pkt = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)msg,
        .len = strlen(msg),
    };

    // Use the synchronous send to all connected clients
    // This iterates through all file descriptors and sends to WebSocket clients
    for (int fd = 0; fd < FD_SETSIZE; fd++) {
        if (httpd_ws_send_frame_async(s_server, fd, &ws_pkt) == ESP_OK) {
            // Frame sent successfully to this client
        }
    }
}

// optional: helper to send a small text reply
static void ws_send_text(httpd_req_t *req, const char *msg)
{
    httpd_ws_frame_t out_frame = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)msg,
        .len = strlen(msg),
    };
    httpd_ws_send_frame(req, &out_frame);
}



esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        // Initial WebSocket handshake (GET request)
        ESP_LOGI(TAG, "WebSocket client connected");
        return ESP_OK;
    }

    // Handle WebSocket frame (text data)
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    // Step 1: get frame length
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ws_recv_frame (query len) failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (ws_pkt.len == 0) {
        // Empty frame or control frame
        return ESP_OK;
    }

    ESP_LOGI(TAG, "WebSocket frame size: %zu bytes", ws_pkt.len);

    // Step 2: allocate buffer and read the actual frame
    char *buf = malloc(ws_pkt.len + 1);
    if (!buf) {
        ESP_LOGE(TAG, "malloc failed for WS frame (size: %zu bytes)", ws_pkt.len);
        return ESP_ERR_NO_MEM;
    }

    ws_pkt.payload = (uint8_t *)buf;
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ws_recv_frame failed: %s", esp_err_to_name(ret));
        free(buf);
        return ret;
    }

    buf[ws_pkt.len] = '\0';
    ESP_LOGI(TAG, "WS recv (%zu bytes): %.100s%s", ws_pkt.len, buf, 
             ws_pkt.len > 100 ? "..." : "");  // Show first 100 chars + ...
    
    // Verify JSON completeness
    size_t open_braces = 0, close_braces = 0;
    for (size_t i = 0; i < ws_pkt.len; i++) {
        if (buf[i] == '{') open_braces++;
        if (buf[i] == '}') close_braces++;
    }
    ESP_LOGI(TAG, "JSON structure check - Open braces: %zu, Close braces: %zu", 
             open_braces, close_braces);

    // Parse JSON command
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        ESP_LOGW(TAG, "invalid JSON");
        ws_send_text(req, "error: invalid json");
        free(buf);
        return ESP_OK;
    }

    cJSON *action = cJSON_GetObjectItem(root, "action");
    if (!action || !cJSON_IsString(action)) {
        ws_send_text(req, "error: missing action");
        cJSON_Delete(root);
        free(buf);
        return ESP_OK;
    }

    // ========== COMMAND: write_json ==========
    if (strcmp(action->valuestring, "write_json") == 0) {
        cJSON *data = cJSON_GetObjectItem(root, "data");
        if (!data) {
            ws_send_text(req, "error: missing data for write_json");
            cJSON_Delete(root);
            free(buf);
            return ESP_OK;
        }

        // Stop current cycle if running
        cycle_skip_current_phase(true);

        // Check available heap memory before processing
        size_t free_heap = esp_get_free_heap_size();
        ESP_LOGI(TAG, "Free heap before processing: %zu bytes", free_heap);

        // The data should be an object with a "phases" field
        if (!cJSON_IsObject(data)) {
            ws_send_text(req, "error: data field must be an object");
            cJSON_Delete(root);
            free(buf);
            return ESP_OK;
        }
        
        cJSON *phases = cJSON_GetObjectItem(data, "phases");
        if (!phases || !cJSON_IsArray(phases)) {
            ws_send_text(req, "error: data.phases must be an array");
            cJSON_Delete(root);
            free(buf);
            return ESP_OK;
        }

        // ===== OPTIMIZED PATH: Load directly from cJSON tree =====
        // Skip serialization and re-parsing. This avoids heap fragmentation.
        // Pass the data object (which contains "phases") so cycle.c can store it
        // and keep string pointers alive for the lifetime of the cycle.
        ESP_LOGI(TAG, "Loading cycle directly from parsed JSON tree (optimized)...");
        esp_err_t load_result = load_cycle_from_cjson(data);

        if (load_result == ESP_OK) {
            ESP_LOGI(TAG, "Cycle loaded successfully from cJSON tree");
            
            // OPTIONAL: Also write to SPIFFS for persistence/backup
            // This is now a lower-priority operation and doesn't block the load
            ESP_LOGI(TAG, "Writing cycle to SPIFFS for persistence...");
            
            // Try to serialize and write, but don't fail if it doesn't work
            char *json_str = cJSON_PrintUnformatted(data);
            if (json_str) {
                size_t json_len = strlen(json_str);
                if (fs_write_file("/spiffs/cycle.json", json_str, json_len) == ESP_OK) {
                    ESP_LOGI(TAG, "cycle.json saved to SPIFFS (%zu bytes) for backup", json_len);
                } else {
                    ESP_LOGW(TAG, "Failed to write to SPIFFS (non-fatal, cycle already loaded)");
                }
                free(json_str);
            } else {
                ESP_LOGW(TAG, "Could not serialize for SPIFFS backup (non-fatal, cycle already loaded)");
            }

            ws_send_text(req, "ok: cycle loaded");
        } else {
            ESP_LOGE(TAG, "Cycle load failed with error: %d", load_result);
            ws_send_text(req, "error: failed to load cycle");
        }

        // NOTE: Do NOT free the root here! cycle.c has stored the data object in g_loaded_cycle_json
        // It will be freed when cycle_unload() is called (e.g., when a new cycle loads)
        free(buf);
        return ESP_OK;
    }
    // ========== COMMAND: start_cycle ==========
    else if (strcmp(action->valuestring, "start_cycle") == 0) {
        if (cycle_is_running()) {
            ws_send_text(req, "error: cycle already running");
        } else {
            ws_send_text(req, "ok: starting cycle");
            cycle_run_loaded_cycle();
        }
    }
    // ========== COMMAND: stop_cycle ==========
    else if (strcmp(action->valuestring, "stop_cycle") == 0) {
        cycle_stop();
        ws_send_text(req, "ok: cycle stopped");
    }
    // ========== COMMAND: skip_phase ==========
    else if (strcmp(action->valuestring, "skip_phase") == 0) {
        cycle_skip_current_phase(true);
        ws_send_text(req, "ok: phase skipped");
    }
    // ========== COMMAND: skip_to_phase ==========
    else if (strcmp(action->valuestring, "skip_to_phase") == 0) {
        cJSON *index = cJSON_GetObjectItem(root, "index");
        if (!index || !cJSON_IsNumber(index)) {
            ws_send_text(req, "error: missing or invalid index for skip_to_phase");
        } else {
            int phase_index = index->valueint;
            cycle_skip_to_phase((size_t)phase_index);
            ws_send_text(req, "ok: skipping to phase");
        }
    }
    // ========== COMMAND: toggle_gpio ==========
    else if (strcmp(action->valuestring, "toggle_gpio") == 0) {
        cJSON *pin = cJSON_GetObjectItem(root, "pin");
        cJSON *state = cJSON_GetObjectItem(root, "state");
        
        if (!pin || !cJSON_IsNumber(pin)) {
            ws_send_text(req, "error: missing or invalid pin number");
        } else if (!state || !cJSON_IsNumber(state)) {
            ws_send_text(req, "error: missing or invalid state (0 or 1)");
        } else {
            int pin_num = pin->valueint;
            int pin_state = state->valueint;
            
            // Set GPIO state
            gpio_set_level((gpio_num_t)pin_num, pin_state);
            
            // Also update gpio_shadow[] so telemetry reflects the change
            extern int gpio_shadow[NUM_COMPONENTS];
            extern const gpio_num_t all_pins[NUM_COMPONENTS];
            for (int i = 0; i < NUM_COMPONENTS; i++) {
                if (all_pins[i] == pin_num) {
                    gpio_shadow[i] = pin_state;
                    break;
                }
            }
            
            char response[100];
            snprintf(response, sizeof(response), "ok: GPIO %d set to %d", pin_num, pin_state);
            ws_send_text(req, response);
            ESP_LOGI(TAG, "GPIO %d toggled to %d", pin_num, pin_state);
        }
    }
    else {
        ws_send_text(req, "error: unknown action");
    }

    cJSON_Delete(root);
    free(buf);
    return ESP_OK;
}

// ====================== TELEMETRY CALLBACK ======================

// Static cache for cycle_data structure (only updated when cycle loads, not every telemetry)
static char *g_cycle_data_cache = NULL;
static size_t g_cycle_data_cache_len = 0;

/**
 * Update the cached cycle_data JSON (called only when a new cycle is loaded)
 * This prevents recreating the same structure 600 times per minute
 */
void ws_update_cycle_data_cache(void)
{
    // Free old cache
    if (g_cycle_data_cache) {
        free(g_cycle_data_cache);
        g_cycle_data_cache = NULL;
        g_cycle_data_cache_len = 0;
    }

    // Build cycle_data array once
    cJSON *cycle_data = cJSON_CreateArray();
    if (!cycle_data) return;

    for (size_t pi = 0; pi < g_num_phases && pi < MAX_PHASES; pi++) {
        Phase *phase = &g_phases[pi];
        cJSON *phase_obj = cJSON_CreateObject();
        
        cJSON_AddStringToObject(phase_obj, "id", phase->id ? phase->id : "");
        cJSON_AddStringToObject(phase_obj, "name", phase->id ? phase->id : "");
        cJSON_AddNumberToObject(phase_obj, "start_time_ms", phase->start_time_ms);
        
        cJSON *components_array = cJSON_AddArrayToObject(phase_obj, "components");
        for (size_t ci = 0; ci < phase->num_components; ci++) {
            PhaseComponent *comp = &phase->components[ci];
            cJSON *comp_obj = cJSON_CreateObject();
            
            cJSON_AddStringToObject(comp_obj, "id", comp->id ? comp->id : "");
            cJSON_AddStringToObject(comp_obj, "label", comp->compId ? comp->compId : "");
            cJSON_AddStringToObject(comp_obj, "compId", comp->compId ? comp->compId : "");
            cJSON_AddNumberToObject(comp_obj, "start_ms", comp->start_ms);
            cJSON_AddNumberToObject(comp_obj, "duration_ms", comp->duration_ms);
            cJSON_AddBoolToObject(comp_obj, "has_motor", comp->has_motor);
            
            cJSON_AddItemToArray(components_array, comp_obj);
        }
        
        cJSON_AddItemToArray(cycle_data, phase_obj);
    }

    // Serialize and cache
    char *json_str = cJSON_PrintUnformatted(cycle_data);
    if (json_str) {
        g_cycle_data_cache = json_str;
        g_cycle_data_cache_len = strlen(json_str);
        ESP_LOGI(TAG, "Cycle data cache updated (%zu bytes)", g_cycle_data_cache_len);
    }

    cJSON_Delete(cycle_data);
}

// Callback function that converts telemetry packet to JSON and broadcasts via WebSocket
// OPTIMIZED: Only sends live telemetry (GPIO, sensors, cycle state), not static cycle_data
static void telemetry_callback(const TelemetryPacket *packet)
{
    if (!packet) return;

    // Build JSON object with ONLY live telemetry data (no cycle_data to reduce allocations)
    cJSON *root = cJSON_CreateObject();
    if (!root) return;

    cJSON_AddStringToObject(root, "type", "telemetry");
    cJSON_AddNumberToObject(root, "packet_timestamp_ms", packet->packet_timestamp_ms);

    // GPIO data
    cJSON *gpio_array = cJSON_AddArrayToObject(root, "gpio");
    for (int i = 0; i < packet->gpio.num_pins; i++) {
        cJSON *gpio_obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(gpio_obj, "pin", packet->gpio.pins[i].pin_number);
        cJSON_AddNumberToObject(gpio_obj, "state", packet->gpio.pins[i].state);
        cJSON_AddItemToArray(gpio_array, gpio_obj);
    }

    // Sensor data
    cJSON *sensors = cJSON_AddObjectToObject(root, "sensors");
    cJSON_AddNumberToObject(sensors, "rpm", packet->sensors.rpm);
    cJSON_AddNumberToObject(sensors, "pressure_freq", packet->sensors.pressure_freq);
    cJSON_AddBoolToObject(sensors, "sensor_error", packet->sensors.sensor_error);

    // Cycle data (current execution state only - not static structure)
    cJSON *cycle = cJSON_AddObjectToObject(root, "cycle");
    cJSON_AddBoolToObject(cycle, "cycle_running", packet->cycle.cycle_running);
    cJSON_AddNumberToObject(cycle, "current_phase_index", packet->cycle.current_phase_index);
    cJSON_AddStringToObject(cycle, "current_phase_name", packet->cycle.current_phase_name);
    cJSON_AddNumberToObject(cycle, "total_phases", packet->cycle.total_phases);
    cJSON_AddNumberToObject(cycle, "phase_elapsed_ms", packet->cycle.phase_elapsed_ms);

    // Serialize to JSON string
    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        ws_broadcast_text(json_str);
        free(json_str);
    }

    cJSON_Delete(root);
}

// ====================== WS CYCLE START ======================

esp_err_t ws_cycle_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 8080;  // choose 8080 so 80 stays free
    
    // Increase WebSocket limits for large JSON messages
    cfg.max_uri_handlers = 16;          // Default: 8
    cfg.max_open_sockets = 7;           // Max allowed: 7 (3 used internally, 4 available)
    cfg.send_wait_timeout = 10;         // Default: 5
    cfg.recv_wait_timeout = 10;         // Default: 5
    cfg.stack_size = 8192;              // Default: 4096 - increase for JSON parsing
    
    s_server_port = cfg.server_port;  

    esp_err_t ret = httpd_start(&s_server, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    httpd_uri_t ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &ws);

    // Log WebSocket endpoint with IP address
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            ESP_LOGI(TAG, "====================================================");
            ESP_LOGI(TAG, "WebSocket endpoint ready:");
            ESP_LOGI(TAG, "  ws://" IPSTR ":%d/ws", IP2STR(&ip_info.ip), cfg.server_port);
            ESP_LOGI(TAG, "====================================================");
        } else {
            ESP_LOGI(TAG, "WebSocket endpoint ready at ws://<esp-ip>:%d/ws", cfg.server_port);
        }
    } else {
        ESP_LOGI(TAG, "WebSocket endpoint ready at ws://<esp-ip>:%d/ws (waiting for IP)", cfg.server_port);
    }

    return ESP_OK;
}

void ws_register_telemetry_callback(void)
{
    telemetry_set_callback(telemetry_callback);
    ESP_LOGI(TAG, "Telemetry callback registered for WebSocket broadcast");
}
