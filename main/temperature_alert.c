#include "temperature_alert.h"

#include "esp_log.h"
#include "nvs.h"

#define TEMPERATURE_ALERT_NAMESPACE "temp_alert"
#define TEMPERATURE_ALERT_NVS_ENABLED "enabled"
#define TEMPERATURE_ALERT_NVS_THRESHOLD "threshold"

static bool s_enabled = true;
static bool s_temporarily_disabled;
static int16_t s_threshold_c10 = TEMPERATURE_ALERT_DEFAULT_THRESHOLD_C10;
static const char *TAG = "temperature_alert";

static esp_err_t save_config(void)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(TEMPERATURE_ALERT_NAMESPACE, NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_u8(h, TEMPERATURE_ALERT_NVS_ENABLED, s_enabled ? 1 : 0);
    if (ret == ESP_OK) {
        ret = nvs_set_i16(h, TEMPERATURE_ALERT_NVS_THRESHOLD, s_threshold_c10);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(h);
    }
    nvs_close(h);
    return ret;
}

esp_err_t temperature_alert_init(void)
{
    s_enabled = true;
    s_temporarily_disabled = false;
    s_threshold_c10 = TEMPERATURE_ALERT_DEFAULT_THRESHOLD_C10;

    nvs_handle_t h;
    esp_err_t ret = nvs_open(TEMPERATURE_ALERT_NAMESPACE, NVS_READONLY, &h);
    if (ret != ESP_OK) {
        ESP_LOGI(TAG, "using defaults: enabled=1 threshold=%d.%dC",
                 s_threshold_c10 / 10, s_threshold_c10 % 10);
        return ESP_OK;
    }

    uint8_t enabled = 1;
    int16_t threshold = TEMPERATURE_ALERT_DEFAULT_THRESHOLD_C10;
    if (nvs_get_u8(h, TEMPERATURE_ALERT_NVS_ENABLED, &enabled) == ESP_OK) {
        s_enabled = enabled != 0;
    }
    if (nvs_get_i16(h, TEMPERATURE_ALERT_NVS_THRESHOLD, &threshold) == ESP_OK &&
        threshold >= TEMPERATURE_ALERT_MIN_THRESHOLD_C10 &&
        threshold <= TEMPERATURE_ALERT_MAX_THRESHOLD_C10) {
        s_threshold_c10 = threshold;
    }
    nvs_close(h);
    ESP_LOGI(TAG, "loaded: enabled=%d threshold=%d.%dC",
             s_enabled, s_threshold_c10 / 10, s_threshold_c10 % 10);
    return ESP_OK;
}

bool temperature_alert_is_enabled(void)
{
    return s_enabled && !s_temporarily_disabled;
}

bool temperature_alert_is_configured_enabled(void)
{
    return s_enabled;
}

bool temperature_alert_is_temporarily_disabled(void)
{
    return s_temporarily_disabled;
}

int16_t temperature_alert_get_threshold_c10(void)
{
    return s_threshold_c10;
}

esp_err_t temperature_alert_set_enabled(bool enabled)
{
    s_enabled = enabled;
    s_temporarily_disabled = false;
    esp_err_t ret = save_config();
    ESP_LOGI(TAG, "saved: enabled=%d threshold=%d.%dC",
             s_enabled, s_threshold_c10 / 10, s_threshold_c10 % 10);
    return ret;
}

void temperature_alert_disable_temporarily(void)
{
    if (s_enabled) {
        s_temporarily_disabled = true;
        ESP_LOGI(TAG, "temporarily disabled until reboot");
    }
}

void temperature_alert_clear_temporary_disable(void)
{
    s_temporarily_disabled = false;
    ESP_LOGI(TAG, "temporary disable cleared");
}

esp_err_t temperature_alert_set_threshold_c10(int16_t threshold_c10)
{
    if (threshold_c10 < TEMPERATURE_ALERT_MIN_THRESHOLD_C10) {
        threshold_c10 = TEMPERATURE_ALERT_MIN_THRESHOLD_C10;
    } else if (threshold_c10 > TEMPERATURE_ALERT_MAX_THRESHOLD_C10) {
        threshold_c10 = TEMPERATURE_ALERT_MAX_THRESHOLD_C10;
    }
    s_threshold_c10 = threshold_c10;
    esp_err_t ret = save_config();
    ESP_LOGI(TAG, "saved: enabled=%d threshold=%d.%dC",
             s_enabled, s_threshold_c10 / 10, s_threshold_c10 % 10);
    return ret;
}

esp_err_t temperature_alert_reset(void)
{
    s_enabled = true;
    s_temporarily_disabled = false;
    s_threshold_c10 = TEMPERATURE_ALERT_DEFAULT_THRESHOLD_C10;
    return save_config();
}
