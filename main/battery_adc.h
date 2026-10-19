#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t battery_adc_init(void);
esp_err_t battery_adc_read_actual_mv(int *mv);
uint8_t battery_adc_percent_from_mv(int mv);

#ifdef __cplusplus
}
#endif
