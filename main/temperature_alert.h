#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TEMPERATURE_ALERT_DEFAULT_THRESHOLD_C10 500
#define TEMPERATURE_ALERT_MIN_THRESHOLD_C10 300
#define TEMPERATURE_ALERT_MAX_THRESHOLD_C10 900

esp_err_t temperature_alert_init(void);
bool temperature_alert_is_enabled(void);
bool temperature_alert_is_configured_enabled(void);
bool temperature_alert_is_temporarily_disabled(void);
int16_t temperature_alert_get_threshold_c10(void);
esp_err_t temperature_alert_set_enabled(bool enabled);
void temperature_alert_disable_temporarily(void);
void temperature_alert_clear_temporary_disable(void);
esp_err_t temperature_alert_set_threshold_c10(int16_t threshold_c10);
esp_err_t temperature_alert_reset(void);

#ifdef __cplusplus
}
#endif
