#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool present;
    bool bmi220;
    uint8_t addr;
    uint8_t chip_id;
    int16_t acc_x;
    int16_t acc_y;
    int16_t acc_z;
    int16_t gyr_x;
    int16_t gyr_y;
    int16_t gyr_z;
    int16_t temp_c10;
} bmi270_sample_t;

esp_err_t bmi270_init(void);
esp_err_t bmi270_read_sample(bmi270_sample_t *sample);
esp_err_t bmi270_read_raw_sample(bmi270_sample_t *sample);
