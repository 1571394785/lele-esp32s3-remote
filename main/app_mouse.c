#include "app_mouse.h"

#include <limits.h>

#include "attitude_history.h"
#include "ble_hid.h"
#include "buttons.h"
#include "nvs.h"

#define MOUSE_DIRECTION_MASK (BUTTON_MASK_UP | BUTTON_MASK_DOWN | BUTTON_MASK_LEFT | BUTTON_MASK_RIGHT)
#define MOUSE_BUTTON_MASK    (BUTTON_MASK_FUNC3 | BUTTON_MASK_FUNC4)
#define MOUSE_SCROLL_MODIFIER_MASK BUTTON_MASK_FUNC2
#define MOUSE_CONTROL_MASK   (MOUSE_DIRECTION_MASK | MOUSE_BUTTON_MASK | MOUSE_SCROLL_MODIFIER_MASK)
#define MOUSE_REPORT_PERIOD_MS 10
#define AIR_MOUSE_GYRO_DEADZONE 180
#define AIR_MOUSE_MAX_STEP 127
#define AIR_MOUSE_BASE_DIVISOR 220
#define AIR_MOUSE_SENS_MIN 10
#define AIR_MOUSE_SENS_MAX 500
#define AIR_MOUSE_SENS_DEFAULT 100
#define AIR_MOUSE_WHEEL_SENS_DEFAULT 100
#define AIR_MOUSE_ACCEL_MIN 0
#define AIR_MOUSE_ACCEL_MAX 500
#define AIR_MOUSE_ACCEL_DEFAULT 0
#define AIR_MOUSE_ACCEL_SCALE_MAX 2000
#define AIR_MOUSE_NVS_NAMESPACE "remote"
#define AIR_MOUSE_NVS_KEY_X "airx"
#define AIR_MOUSE_NVS_KEY_Y "airy"
#define AIR_MOUSE_NVS_KEY_ACCEL "airaccel"
#define AIR_MOUSE_NVS_KEY_WHEEL "airwheel"

static uint16_t s_air_x_sensitivity = AIR_MOUSE_SENS_DEFAULT;
static uint16_t s_air_y_sensitivity = AIR_MOUSE_SENS_DEFAULT;
static uint16_t s_air_accel = AIR_MOUSE_ACCEL_DEFAULT;
static uint16_t s_air_wheel_sensitivity = AIR_MOUSE_WHEEL_SENS_DEFAULT;

static uint8_t mouse_buttons_from_mask(uint16_t mask)
{
    uint8_t buttons = 0;
    if (mask & BUTTON_MASK_FUNC4) buttons |= 0x01; // Left button
    if (mask & BUTTON_MASK_FUNC3) buttons |= 0x02; // Right button
    return buttons;
}

static int8_t clamp_i8(int32_t value, int32_t limit)
{
    if (value > limit) return (int8_t)limit;
    if (value < -limit) return (int8_t)-limit;
    return (int8_t)value;
}

static int32_t deadzone_i32(int32_t value)
{
    if (value > AIR_MOUSE_GYRO_DEADZONE) {
        return value - AIR_MOUSE_GYRO_DEADZONE;
    }
    if (value < -AIR_MOUSE_GYRO_DEADZONE) {
        return value + AIR_MOUSE_GYRO_DEADZONE;
    }
    return 0;
}

static uint16_t clamp_sensitivity(uint16_t sensitivity)
{
    if (sensitivity < AIR_MOUSE_SENS_MIN || sensitivity > AIR_MOUSE_SENS_MAX) {
        return AIR_MOUSE_SENS_DEFAULT;
    }
    return sensitivity;
}

static uint16_t clamp_accel(uint16_t accel)
{
    if (accel < AIR_MOUSE_ACCEL_MIN || accel > AIR_MOUSE_ACCEL_MAX) {
        return AIR_MOUSE_ACCEL_DEFAULT;
    }
    return accel;
}

static void save_air_mouse_config(void)
{
    nvs_handle_t h;
    if (nvs_open(AIR_MOUSE_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u16(h, AIR_MOUSE_NVS_KEY_X, s_air_x_sensitivity);
        nvs_set_u16(h, AIR_MOUSE_NVS_KEY_Y, s_air_y_sensitivity);
        nvs_set_u16(h, AIR_MOUSE_NVS_KEY_ACCEL, s_air_accel);
        nvs_set_u16(h, AIR_MOUSE_NVS_KEY_WHEEL, s_air_wheel_sensitivity);
        nvs_commit(h);
        nvs_close(h);
    }
}

static int32_t apply_air_sensitivity(int32_t value, uint16_t sensitivity)
{
    return (value * (int32_t)clamp_sensitivity(sensitivity)) / 100;
}

static int32_t apply_air_accel(int32_t value)
{
    int32_t magnitude = value < 0 ? -value : value;
    int32_t scale_percent = 100 + ((int32_t)s_air_accel * magnitude) / 100;

    if (scale_percent > AIR_MOUSE_ACCEL_SCALE_MAX) {
        scale_percent = AIR_MOUSE_ACCEL_SCALE_MAX;
    }

    return (value * scale_percent) / 100;
}

void app_mouse_config_init(void)
{
    uint16_t x = AIR_MOUSE_SENS_DEFAULT;
    uint16_t y = AIR_MOUSE_SENS_DEFAULT;
    uint16_t accel = AIR_MOUSE_ACCEL_DEFAULT;
    uint16_t wheel = AIR_MOUSE_WHEEL_SENS_DEFAULT;
    nvs_handle_t h;
    if (nvs_open(AIR_MOUSE_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u16(h, AIR_MOUSE_NVS_KEY_X, &x);
        nvs_get_u16(h, AIR_MOUSE_NVS_KEY_Y, &y);
        nvs_get_u16(h, AIR_MOUSE_NVS_KEY_ACCEL, &accel);
        nvs_get_u16(h, AIR_MOUSE_NVS_KEY_WHEEL, &wheel);
        nvs_close(h);
    }
    s_air_x_sensitivity = clamp_sensitivity(x);
    s_air_y_sensitivity = clamp_sensitivity(y);
    s_air_accel = clamp_accel(accel);
    s_air_wheel_sensitivity = clamp_sensitivity(wheel);
}

uint16_t app_mouse_get_air_x_sensitivity(void)
{
    return s_air_x_sensitivity;
}

uint16_t app_mouse_get_air_y_sensitivity(void)
{
    return s_air_y_sensitivity;
}

uint16_t app_mouse_get_air_accel(void)
{
    return s_air_accel;
}

uint16_t app_mouse_get_air_wheel_sensitivity(void)
{
    return s_air_wheel_sensitivity;
}

void app_mouse_set_air_x_sensitivity(uint16_t sensitivity)
{
    s_air_x_sensitivity = clamp_sensitivity(sensitivity);
    save_air_mouse_config();
}

void app_mouse_set_air_y_sensitivity(uint16_t sensitivity)
{
    s_air_y_sensitivity = clamp_sensitivity(sensitivity);
    save_air_mouse_config();
}

void app_mouse_set_air_accel(uint16_t accel)
{
    s_air_accel = clamp_accel(accel);
    save_air_mouse_config();
}

void app_mouse_set_air_wheel_sensitivity(uint16_t sensitivity)
{
    s_air_wheel_sensitivity = clamp_sensitivity(sensitivity);
    save_air_mouse_config();
}

void app_mouse_state_init(app_mouse_state_t *state)
{
    if (state == NULL) return;

    state->last_control_mask = UINT16_MAX;
    state->direction_mask = 0;
    state->direction_start_ms = 0;
    state->last_move_ms = 0;
    state->last_air_sample_ms = 0;
    state->air_x_accum = 0;
    state->air_y_accum = 0;
    air_mouse_frame_reset(&state->air_frame);
    air_mouse_scroll_reset(&state->air_scroll);
    state->air_scroll_mode = false;
    state->active = false;
    state->suppress_until_release = false;
}

void app_mouse_enter(app_mouse_state_t *state, uint16_t initial_mask)
{
    if (state == NULL) return;

    state->last_control_mask = initial_mask & MOUSE_CONTROL_MASK;
    state->direction_mask = 0;
    state->direction_start_ms = 0;
    state->last_move_ms = 0;
    state->last_air_sample_ms = 0;
    state->air_x_accum = 0;
    state->air_y_accum = 0;
    air_mouse_frame_reset(&state->air_frame);
    air_mouse_scroll_reset(&state->air_scroll);
    state->air_scroll_mode = false;
    state->active = true;
    state->suppress_until_release = initial_mask != 0;
}

esp_err_t app_mouse_leave(app_mouse_state_t *state)
{
    if (state == NULL || !state->active) return ESP_OK;

    state->active = false;
    state->last_control_mask = UINT16_MAX;
    state->direction_mask = 0;
    state->last_air_sample_ms = 0;
    state->air_x_accum = 0;
    state->air_y_accum = 0;
    air_mouse_frame_reset(&state->air_frame);
    air_mouse_scroll_reset(&state->air_scroll);
    state->air_scroll_mode = false;
    return ble_hid_send_mouse_report(0, 0, 0, 0);
}

esp_err_t app_mouse_handle(app_mouse_state_t *state, uint16_t button_mask, uint32_t now_ms)
{
    if (state == NULL || !state->active) return ESP_ERR_INVALID_STATE;

    uint16_t control_mask = button_mask & MOUSE_CONTROL_MASK;
    if (state->suppress_until_release) {
        if (control_mask == 0) {
            state->suppress_until_release = false;
            state->last_control_mask = 0;
        }
        return ESP_OK;
    }

    bool scroll_mode = (control_mask & MOUSE_SCROLL_MODIFIER_MASK) != 0;
    uint16_t directions = scroll_mode ? 0 : (control_mask & MOUSE_DIRECTION_MASK);
    if (scroll_mode != state->air_scroll_mode) {
        state->air_x_accum = 0;
        state->air_y_accum = 0;
        air_mouse_scroll_reset(&state->air_scroll);
        state->air_scroll_mode = scroll_mode;
    }
    if (directions != state->direction_mask) {
        state->direction_mask = directions;
        state->direction_start_ms = now_ms;
        state->last_move_ms = 0;
    }

    attitude_sample_t attitude = {0};
    bool have_attitude = attitude_history_latest(&attitude);
    bool control_changed = control_mask != state->last_control_mask;
    bool have_new_attitude = have_attitude && attitude.timestamp_ms != state->last_air_sample_ms;
    bool move_due = (directions != 0 || have_new_attitude) &&
                    (state->last_move_ms == 0 || now_ms - state->last_move_ms >= MOUSE_REPORT_PERIOD_MS);
    if (!control_changed && !move_due) return ESP_OK;

    int32_t dx_value = 0;
    int32_t dy_value = 0;
    int32_t wheel_value = 0;
    if (move_due && directions != 0) {
        uint32_t held_ms = now_ms - state->direction_start_ms;
        uint32_t speed_value = 2 + held_ms / 180;
        if (speed_value > 18) speed_value = 18;
        int32_t speed = (int32_t)speed_value;

        if (directions & BUTTON_MASK_LEFT) dx_value -= speed;
        if (directions & BUTTON_MASK_RIGHT) dx_value += speed;
        if (directions & BUTTON_MASK_UP) dy_value -= speed;
        if (directions & BUTTON_MASK_DOWN) dy_value += speed;
    }

    if (move_due) {
        if (have_new_attitude) {
            int32_t horizontal = 0;
            int32_t vertical = 0;
            air_mouse_frame_update(&state->air_frame, &attitude.imu,
                                   attitude.timestamp_ms, &horizontal, &vertical);
            int32_t air_y_value = deadzone_i32(vertical);
            if (scroll_mode) {
                uint32_t elapsed_ms = state->last_air_sample_ms == 0 ?
                                      MOUSE_REPORT_PERIOD_MS : attitude.timestamp_ms - state->last_air_sample_ms;
                wheel_value = air_mouse_scroll_update(&state->air_scroll,
                    apply_air_sensitivity(air_y_value, s_air_wheel_sensitivity),
                    elapsed_ms, ble_hid_get_wheel_resolution());
            } else {
                state->air_y_accum += apply_air_accel(apply_air_sensitivity(
                    air_y_value, s_air_y_sensitivity));
                state->air_x_accum += apply_air_accel(apply_air_sensitivity(
                    deadzone_i32(horizontal), s_air_x_sensitivity));
                dx_value += state->air_x_accum / AIR_MOUSE_BASE_DIVISOR;
                dy_value += state->air_y_accum / AIR_MOUSE_BASE_DIVISOR;
                state->air_x_accum %= AIR_MOUSE_BASE_DIVISOR;
                state->air_y_accum %= AIR_MOUSE_BASE_DIVISOR;
            }
            state->last_air_sample_ms = attitude.timestamp_ms;
        }
        state->last_move_ms = now_ms;
    }

    if (!control_changed && dx_value == 0 && dy_value == 0 && wheel_value == 0) {
        return ESP_OK;
    }

    esp_err_t ret = ble_hid_send_mouse_report_precise(mouse_buttons_from_mask(control_mask),
                                     clamp_i8(dx_value, AIR_MOUSE_MAX_STEP),
                                     clamp_i8(dy_value, AIR_MOUSE_MAX_STEP),
                                     (int8_t)wheel_value);
    // A button change deferred while the host subscribes must be retried.
    if (ret == ESP_OK && ble_hid_mouse_report_ready()) state->last_control_mask = control_mask;
    return ret;
}
