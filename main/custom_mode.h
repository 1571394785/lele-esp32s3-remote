#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "buttons.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t modifier;
    uint8_t keycode;
} custom_key_combo_t;

#define CUSTOM_SHORTCUT_MAX_LEN 127
#define CUSTOM_SEQUENCE_MAX 128
#define CUSTOM_WIFI_SSID_MAX_LEN 32
#define CUSTOM_WIFI_PASSWORD_MAX_LEN 64
#define CUSTOM_HTTP_URL_MAX_LEN 383
#define CUSTOM_HTTP_BODY_MAX_LEN 511

typedef enum {
    CUSTOM_ACTION_SHORTCUT = 0,
    CUSTOM_ACTION_HTTP,
} custom_action_type_t;

typedef enum {
    CUSTOM_HTTP_GET = 0,
    CUSTOM_HTTP_POST,
} custom_http_method_t;

typedef struct {
    custom_action_type_t type;
    custom_http_method_t method;
    char shortcut[CUSTOM_SHORTCUT_MAX_LEN + 1];
    char url[CUSTOM_HTTP_URL_MAX_LEN + 1];
    char body[CUSTOM_HTTP_BODY_MAX_LEN + 1];
} custom_button_config_t;

esp_err_t custom_mode_init(void);
esp_err_t custom_mode_set_shortcut(button_key_t key, const char *shortcut);
esp_err_t custom_mode_set_wifi(const char *ssid, const char *password);
esp_err_t custom_mode_set_button_config(button_key_t key, const custom_button_config_t *config);
esp_err_t custom_mode_clear_all(void);
const char *custom_mode_get_shortcut(button_key_t key);
const char *custom_mode_get_wifi_ssid(void);
const char *custom_mode_get_wifi_password(void);
custom_action_type_t custom_mode_get_action_type(button_key_t key);
bool custom_mode_get_button_config(button_key_t key, custom_button_config_t *config);
bool custom_mode_get_combo(button_key_t key, custom_key_combo_t *combo);
bool custom_mode_get_sequence(button_key_t key, custom_key_combo_t *sequence, size_t max_count, size_t *count);

#ifdef __cplusplus
}
#endif
