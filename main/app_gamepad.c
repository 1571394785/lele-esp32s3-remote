#include "app_gamepad.h"

#include "ble_hid.h"
#include "buttons.h"

static uint16_t gamepad_buttons_from_mask(uint16_t mask)
{
    uint16_t buttons = 0;

    if (mask & BUTTON_MASK_FUNC3) buttons |= 0x01; // A
    if (mask & BUTTON_MASK_FUNC1) buttons |= 0x02; // B
    if (mask & BUTTON_MASK_FUNC2) buttons |= 0x04; // X
    if (mask & BUTTON_MASK_FUNC4) buttons |= 0x08; // Y
    return buttons;
}

static uint8_t gamepad_hat_from_mask(uint16_t mask)
{
    bool up = (mask & BUTTON_MASK_UP) != 0;
    bool down = (mask & BUTTON_MASK_DOWN) != 0;
    bool left = (mask & BUTTON_MASK_LEFT) != 0;
    bool right = (mask & BUTTON_MASK_RIGHT) != 0;

    if (up && right && !down && !left) return 1;
    if (down && right && !up && !left) return 3;
    if (down && left && !up && !right) return 5;
    if (up && left && !down && !right) return 7;
    if (up && !down) return 0;
    if (right && !left) return 2;
    if (down && !up) return 4;
    if (left && !right) return 6;
    return 8; // neutral
}

void app_gamepad_state_init(app_gamepad_state_t *state)
{
    if (state == NULL) {
        return;
    }

    state->last_mask = UINT16_MAX;
    state->menu_combo_start_ms = 0;
    state->menu_combo_done = false;
    state->suppress_menu_keys_until_release = false;
}

esp_err_t app_gamepad_handle_mode(app_gamepad_state_t *state, uint16_t button_mask,
                                  uint32_t now_ms, mode_view_t *view, bool *manager_changed)
{
    if (state == NULL || view == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    bool menu_combo = (button_mask & (BUTTON_MASK_FUNC1 | BUTTON_MASK_FUNC2)) ==
                      (BUTTON_MASK_FUNC1 | BUTTON_MASK_FUNC2);

    if (menu_combo) {
        if (state->menu_combo_start_ms == 0) {
            state->menu_combo_start_ms = now_ms;
        } else if (!state->menu_combo_done && now_ms - state->menu_combo_start_ms >= 3000) {
            ESP_ERROR_CHECK(ble_hid_send_gamepad(0, 8));
            state->last_mask = 0;
            mode_manager_enter_menu(view);
            state->menu_combo_done = true;
            state->suppress_menu_keys_until_release = true;
            if (manager_changed != NULL) {
                *manager_changed = true;
            }
        }
    } else {
        state->menu_combo_start_ms = 0;
        state->menu_combo_done = false;
    }

    if (view->screen == APP_SCREEN_MODE && button_mask != state->last_mask) {
        ESP_ERROR_CHECK(ble_hid_send_gamepad(gamepad_buttons_from_mask(button_mask),
                                             gamepad_hat_from_mask(button_mask)));
        state->last_mask = button_mask;
    }

    return ESP_OK;
}

void app_gamepad_filter_menu_key(app_gamepad_state_t *state, uint16_t button_mask, button_key_t *key)
{
    if (state == NULL || key == NULL || !state->suppress_menu_keys_until_release) {
        return;
    }

    if (button_mask == 0) {
        state->suppress_menu_keys_until_release = false;
    } else {
        *key = BUTTON_KEY_NONE;
    }
}

void app_gamepad_reset_if_inactive(app_gamepad_state_t *state, const mode_view_t *view)
{
    if (state == NULL || view == NULL) {
        return;
    }

    if (view->screen != APP_SCREEN_MODE || !view->is_gamepad_mode) {
        state->menu_combo_start_ms = 0;
        state->menu_combo_done = false;
        state->last_mask = UINT16_MAX;
    }
}
