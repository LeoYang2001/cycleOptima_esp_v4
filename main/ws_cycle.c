#include "ws_cycle.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_https_server.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "cJSON.h"

#include "fs.h"           // fs_write_file(...)
#include "cycle.h"        // cycle_load_from_json_str(...), cycle_run_loaded_cycle(...)
#include "telemetry.h"    // TelemetryPacket, telemetry_set_callback()

static const char *TAG = "ws_cycle";

static httpd_handle_t s_server = NULL;

static uint16_t s_server_port = 0;  // Store port for external logging

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

    // Step 2: allocate buffer and read the actual frame
    char *buf = malloc(ws_pkt.len + 1);
    if (!buf) {
        ESP_LOGE(TAG, "malloc failed for WS frame");
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
    ESP_LOGI(TAG, "WS recv: %s", buf);

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
        } else {
            // Stop current cycle if running
            cycle_skip_current_phase(true);

            // Serialize data to JSON string
            char *data_str = cJSON_PrintUnformatted(data);
            if (data_str) {
                // Write to SPIFFS
                if (fs_write_file("/spiffs/cycle.json", data_str, strlen(data_str)) == ESP_OK) {
                    ESP_LOGI(TAG, "cycle.json updated via websocket");
                } else {
                    ESP_LOGE(TAG, "failed to write cycle.json to spiffs");
                }

                // Load into RAM (stay IDLE until start_cycle)
                if (cycle_load_from_json_str(data_str) == ESP_OK) {
                    ws_send_text(req, "ok: json written and loaded");
                } else {
                    ws_send_text(req, "error: json written but failed to load");
                }
                free(data_str);
            } else {
                ws_send_text(req, "error: cannot serialize data");
            }
        }
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

// Callback function that converts telemetry packet to JSON and broadcasts via WebSocket
static void telemetry_callback(const TelemetryPacket *packet)
{
    if (!packet) return;

    // Build JSON object with telemetry data
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

    // Cycle data (current execution state)
    cJSON *cycle = cJSON_AddObjectToObject(root, "cycle");
    cJSON_AddBoolToObject(cycle, "cycle_running", packet->cycle.cycle_running);
    cJSON_AddNumberToObject(cycle, "current_phase_index", packet->cycle.current_phase_index);
    cJSON_AddStringToObject(cycle, "current_phase_name", packet->cycle.current_phase_name);
    cJSON_AddNumberToObject(cycle, "total_phases", packet->cycle.total_phases);
    cJSON_AddNumberToObject(cycle, "phase_elapsed_ms", packet->cycle.phase_elapsed_ms);
    cJSON_AddNumberToObject(cycle, "phase_total_duration_ms", packet->cycle.phase_total_duration_ms);
    cJSON_AddNumberToObject(cycle, "cycle_start_time_ms", packet->cycle.cycle_start_time_ms);

    // Cycledata: full cycle structure with all phases and components
    cJSON *cycle_data = cJSON_AddArrayToObject(root, "cycle_data");
    
    for (size_t pi = 0; pi < packet->cycle.total_phases && pi < MAX_PHASES; pi++) {
        Phase *phase = &g_phases[pi];
        cJSON *phase_obj = cJSON_CreateObject();
        
        // Phase metadata
        cJSON_AddStringToObject(phase_obj, "id", phase->id ? phase->id : "");
        cJSON_AddStringToObject(phase_obj, "name", phase->name ? phase->name : "");
        cJSON_AddStringToObject(phase_obj, "color", phase->color ? phase->color : "");
        cJSON_AddNumberToObject(phase_obj, "start_time_ms", phase->start_time_ms);
        
        // Phase components
        cJSON *components_array = cJSON_AddArrayToObject(phase_obj, "components");
        for (size_t ci = 0; ci < phase->num_components; ci++) {
            PhaseComponent *comp = &phase->components[ci];
            cJSON *comp_obj = cJSON_CreateObject();
            
            cJSON_AddStringToObject(comp_obj, "id", comp->id ? comp->id : "");
            cJSON_AddStringToObject(comp_obj, "label", comp->label ? comp->label : "");
            cJSON_AddStringToObject(comp_obj, "compId", comp->compId ? comp->compId : "");
            cJSON_AddNumberToObject(comp_obj, "start_ms", comp->start_ms);
            cJSON_AddNumberToObject(comp_obj, "duration_ms", comp->duration_ms);
            cJSON_AddBoolToObject(comp_obj, "has_motor", comp->has_motor);
            
            cJSON_AddItemToArray(components_array, comp_obj);
        }
        
        cJSON_AddItemToArray(cycle_data, phase_obj);
    }

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
