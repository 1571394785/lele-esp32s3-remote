#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "air_mouse_frame.h"
#include "air_mouse_scroll.h"

typedef struct {
    uint16_t last_control_mask;
    uint16_t direction_mask;
    uint32_t direction_start_ms;
    uint32_t last_move_ms;
    uint32_t last_air_sample_ms;
    int32_t air_x_accum;
    int32_t air_y_accum;
    air_mouse_frame_t air_frame;
    air_mouse_scroll_t air_scroll;
    bool air_scroll_mode;
    bool active;
    bool suppress_until_release;
} app_mouse_state_t;

void app_mouse_state_init(app_mouse_state_t *state);
void app_mouse_config_init(void);
uint16_t app_mouse_get_air_x_sensitivity(void);
uint16_t app_mouse_get_air_y_sensitivity(void);
uint16_t app_mouse_get_air_accel(void);
uint16_t app_mouse_get_air_wheel_sensitivity(void);
void app_mouse_set_air_x_sensitivity(uint16_t sensitivity);
void app_mouse_set_air_y_sensitivity(uint16_t sensitivity);
void app_mouse_set_air_accel(uint16_t accel);
void app_mouse_set_air_wheel_sensitivity(uint16_t sensitivity);
void app_mouse_enter(app_mouse_state_t *state, uint16_t initial_mask);
esp_err_t app_mouse_leave(app_mouse_state_t *state);
esp_err_t app_mouse_handle(app_mouse_state_t *state, uint16_t button_mask, uint32_t now_ms);
