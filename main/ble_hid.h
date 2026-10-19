#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_HID_DEVICE_SLOT_COUNT 3
#define BLE_HID_WHEEL_FEATURE_REPORT_ID 5

typedef struct {
    bool valid;
    uint8_t addr[6];
    uint8_t addr_type;
} ble_hid_device_slot_t;

/** Mouse motion step types */
#define MOUSE_STEP_MOVE    0  // Relative move (dx, dy)
#define MOUSE_STEP_SCROLL  1  // Wheel scroll
#define MOUSE_STEP_RESET   2  // Send zero report
#define MOUSE_STEP_DELAY   3  // Wait milliseconds

/** A single step in a mouse motion pipeline */
typedef struct {
    uint8_t  type;          // MOUSE_STEP_xxx
    uint8_t  buttons;       // Button mask (bit0=left)
    int8_t   dx;            // Relative X movement
    int8_t   dy;            // Relative Y movement
    int8_t   wheel;         // Wheel delta
    uint8_t  repeat;        // Repeat count
    uint32_t delay_ms;      // Delay AFTER the step(s)
} mouse_step_t;

esp_err_t ble_hid_init(void);
void ble_hid_poll(void);
bool ble_hid_is_connected(void);
const char *ble_hid_get_connected_addr(void);
esp_err_t ble_hid_send_key(uint8_t keycode);
esp_err_t ble_hid_send_key_combo(uint8_t modifier, uint8_t keycode);
esp_err_t ble_hid_keyboard_press(uint8_t modifier, uint8_t keycode);
esp_err_t ble_hid_keyboard_release(void);
esp_err_t ble_hid_send_consumer(uint16_t usage);
esp_err_t ble_hid_send_gamepad(uint16_t buttons, uint8_t hat);
bool ble_hid_is_full_mode(void);
esp_err_t ble_hid_toggle_hid_mode(void);
esp_err_t ble_hid_disconnect(void);
esp_err_t ble_hid_enable_connection(void);
esp_err_t ble_hid_enter_pairing_mode(void);
esp_err_t ble_hid_clear_pairing(void);
esp_err_t ble_hid_select_device_slot(uint8_t slot);
esp_err_t ble_hid_switch_device_slot(uint8_t slot);
esp_err_t ble_hid_connect_selected_device(void);
esp_err_t ble_hid_pair_selected_device(void);
uint8_t ble_hid_get_selected_device_slot(void);
bool ble_hid_get_device_slot(uint8_t slot, ble_hid_device_slot_t *out);
bool ble_hid_is_pairing_mode(void);

/** Execute a mouse motion pipeline defined by steps array */
esp_err_t ble_hid_mouse_exec(const mouse_step_t *steps, size_t count);
esp_err_t ble_hid_send_mouse_report(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel);
// Host-negotiated units per wheel detent (1 or 120).
uint16_t ble_hid_get_wheel_resolution(void);
bool ble_hid_mouse_report_ready(void);
esp_err_t ble_hid_send_mouse_report_precise(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel);

/** Convenience: vertical scroll with center-positioning preamble */
esp_err_t ble_hid_drag_vertical(bool upward);

#ifdef __cplusplus
}
#endif
