#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "buttons.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CUSTOM_HTTP_STATE_IDLE = 0,
    CUSTOM_HTTP_STATE_STARTING_WIFI,
    CUSTOM_HTTP_STATE_CONNECTING_WIFI,
    CUSTOM_HTTP_STATE_SENDING,
    CUSTOM_HTTP_STATE_DISCONNECTING,
    CUSTOM_HTTP_STATE_SUCCESS,
    CUSTOM_HTTP_STATE_FAILED,
} custom_http_state_t;

typedef struct {
    custom_http_state_t state;
    int http_status;
    char detail[64];
} custom_http_status_t;

esp_err_t web_control_start(void);
esp_err_t web_control_stop(void);
bool web_control_is_running(void);
esp_err_t web_control_start_custom_http(button_key_t key);
void web_control_get_custom_http_status(custom_http_status_t *status);

#ifdef __cplusplus
}
#endif
