#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "mode_manager.h"

typedef struct {
    uint16_t last_mask;
    uint32_t menu_combo_start_ms;
    bool menu_combo_done;
    bool suppress_menu_keys_until_release;
} app_gamepad_state_t;

void app_gamepad_state_init(app_gamepad_state_t *state);
esp_err_t app_gamepad_handle_mode(app_gamepad_state_t *state, uint16_t button_mask,
                                  uint32_t now_ms, mode_view_t *view, bool *manager_changed);
void app_gamepad_filter_menu_key(app_gamepad_state_t *state, uint16_t button_mask, button_key_t *key);
void app_gamepad_reset_if_inactive(app_gamepad_state_t *state, const mode_view_t *view);
