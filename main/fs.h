// fs.h
#pragma once

#include "esp_err.h"

// mount SPIFFS at /spiffs
esp_err_t fs_init_spiffs(void);

// read an entire file into a heap buffer
// remember to free() the returned pointer
char *fs_read_file(const char *path);
