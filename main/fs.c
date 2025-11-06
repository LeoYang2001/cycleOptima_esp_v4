// fs.c
#include "fs.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "fs";

esp_err_t fs_init_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,      // first SPIFFS partition
        .max_files = 5,
        .format_if_mount_failed = false
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS mounted. total=%d, used=%d", total, used);
    } else {
        ESP_LOGW(TAG, "SPIFFS mounted but esp_spiffs_info failed: %s", esp_err_to_name(ret));
    }

    return ESP_OK;
}

char *fs_read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file: %s", path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc(size + 1);
    if (!buf) {
        ESP_LOGE(TAG, "malloc failed for file: %s", path);
        fclose(f);
        return NULL;
    }

    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
}


esp_err_t fs_write_file(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        return ESP_FAIL;
    }
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    return (w == len) ? ESP_OK : ESP_FAIL;
}