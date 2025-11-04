// wifi_sta.c
#include "wifi_sta.h"

#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"

static const char *TAG = "wifi_sta";

// TODO: put your router SSID/PASS here for now
#define WIFI_SSID "CycleOptima-dedicate"
#define WIFI_PASS "Alpha2700"
#define WIFI_MAX_RETRY 5

static int s_retry_num = 0;
static bool s_connected = false;

// event handler
static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG, "retry to connect to the AP");
        } else {
            ESP_LOGE(TAG, "connect to the AP fail");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_connected = true;
    }
}

bool wifi_sta_is_connected(void)
{
    return s_connected;
}

static esp_err_t wifi_nvs_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

esp_err_t wifi_sta_init_and_connect(void)
{
    ESP_ERROR_CHECK(wifi_nvs_init());

    // network interfaces
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // register event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
                        WIFI_EVENT,
                        ESP_EVENT_ANY_ID,
                        &wifi_event_handler,
                        NULL,
                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
                        IP_EVENT,
                        IP_EVENT_STA_GOT_IP,
                        &wifi_event_handler,
                        NULL,
                        NULL));

    wifi_config_t wifi_config = {
        .sta = {
            // ESP-IDF needs 32-byte ssid/pass max; make sure to zero-init
        },
    };
    // copy ssid and pass
    strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password));

    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    // if your AP is open, you can remove the line above

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    // connection will be attempted in the event handler
    return ESP_OK;
}
