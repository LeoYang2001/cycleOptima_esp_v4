// ws_cycle.c
#include "ws_cycle.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_https_server.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "cJSON.h"

#include "fs.h"      // fs_write_file(...)
#include "cycle.h"   // cycle_load_from_json_str(...), cycle_run_loaded_cycle(...)

static const char *TAG = "ws_cycle";

static httpd_handle_t s_server = NULL;

static uint16_t s_server_port = 0;  // Store port for external logging

uint16_t ws_cycle_get_port(void)
{
    return s_server_port;
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
        cycle_skip_current_phase(true);
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
    else {
        ws_send_text(req, "error: unknown action");
    }

    cJSON_Delete(root);
    free(buf);
    return ESP_OK;
}
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
