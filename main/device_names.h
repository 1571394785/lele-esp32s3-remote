#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#define DEVICE_NAME_COUNT 3
#define DEVICE_NAME_BYTES 48
#define DEVICE_NAME_CHARS 16
esp_err_t device_names_init(void);
esp_err_t device_names_save(const char names[DEVICE_NAME_COUNT][DEVICE_NAME_BYTES + 1]);
esp_err_t device_names_reset(void);
void device_names_get(uint8_t slot, char *out, size_t size);
