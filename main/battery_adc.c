#include "battery_adc.h"

#include <stdbool.h>

#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"

#define BATTERY_ADC_GPIO 3
#define BATTERY_ADC_ATTEN ADC_ATTEN_DB_12
#define BATTERY_ADC_BITWIDTH ADC_BITWIDTH_DEFAULT
#define BATTERY_DIVIDER_MULTIPLIER 2
#define BATTERY_EMPTY_MV 3100
#define BATTERY_FULL_MV 4100

static const char *TAG = "battery_adc";

static adc_oneshot_unit_handle_t s_adc_unit;
static adc_cali_handle_t s_cali;
static adc_unit_t s_unit_id;
static adc_channel_t s_channel;
static bool s_calibrated;

esp_err_t battery_adc_init(void)
{
    ESP_RETURN_ON_ERROR(adc_oneshot_io_to_channel(BATTERY_ADC_GPIO, &s_unit_id, &s_channel),
                        TAG, "GPIO%d is not ADC capable", BATTERY_ADC_GPIO);

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = s_unit_id,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_adc_unit), TAG, "adc unit init failed");

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = BATTERY_ADC_ATTEN,
        .bitwidth = BATTERY_ADC_BITWIDTH,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc_unit, s_channel, &chan_cfg),
                        TAG, "adc channel config failed");

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = s_unit_id,
        .chan = s_channel,
        .atten = BATTERY_ADC_ATTEN,
        .bitwidth = BATTERY_ADC_BITWIDTH,
    };
    esp_err_t cali_ret = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali);
    s_calibrated = cali_ret == ESP_OK;
    if (!s_calibrated) {
        ESP_LOGW(TAG, "adc calibration unavailable: %s", esp_err_to_name(cali_ret));
    }
#endif

    ESP_LOGI(TAG, "battery adc initialized: gpio=%d unit=%d channel=%d", BATTERY_ADC_GPIO, s_unit_id, s_channel);
    return ESP_OK;
}

esp_err_t battery_adc_read_actual_mv(int *mv)
{
    if (mv == NULL || s_adc_unit == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int pin_mv = 0;
    if (s_calibrated) {
        ESP_RETURN_ON_ERROR(adc_oneshot_get_calibrated_result(s_adc_unit, s_cali, s_channel, &pin_mv),
                            TAG, "adc calibrated read failed");
    } else {
        int raw = 0;
        ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc_unit, s_channel, &raw), TAG, "adc raw read failed");
        pin_mv = raw * 3300 / 4095 + 0.06;
    }

    *mv = pin_mv * BATTERY_DIVIDER_MULTIPLIER;
    return ESP_OK;
}

uint8_t battery_adc_percent_from_mv(int mv)
{
    if (mv <= BATTERY_EMPTY_MV) {
        return 0;
    }
    if (mv >= BATTERY_FULL_MV) {
        return 100;
    }

    return (uint8_t)(((mv - BATTERY_EMPTY_MV) * 100 + (BATTERY_FULL_MV - BATTERY_EMPTY_MV) / 2) /
                     (BATTERY_FULL_MV - BATTERY_EMPTY_MV));
}
