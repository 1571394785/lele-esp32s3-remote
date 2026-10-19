#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "buttons.h"

#define MODE_COUNT 7
#define MENU_ITEM_MAX 10

typedef enum {
    APP_SCREEN_MODE = 0,
    APP_SCREEN_MENU,
} app_screen_t;

typedef struct {
    app_screen_t screen;
    const char *title;
    const char *line1;
    const char *line2;
    bool show_addr;            // show BLE peer address on menu screen
    bool show_status;          // show status page instead of menu items
    bool show_web_control;     // show web control instructions instead of menu items
    bool show_temperature_alert_config; // show temperature alert settings
    bool show_about;           // show about page with QR code
    bool show_hid_confirm;     // show HID mode switch confirmation
    bool show_air_mouse_config; // show air mouse sensitivity editor
    bool show_air_wheel_config; // show air wheel sensitivity editor
    bool show_data_collection; // show serial CSV data collection controls
    bool show_gesture_inference; // show on-device gesture inference controls
    bool show_devices;         // show device slot list
    bool blink_selected;       // blink current selected menu item
    bool is_gamepad_mode;      // current mode is BLE gamepad
    bool is_mouse_mode;        // current mode is relative mouse
    bool is_temporary_mouse;   // return to previous mode with F2
    bool show_scrollbar;       // menu has more rows than visible area
    bool temperature_alert_enabled;
    bool temperature_alert_temporarily_disabled;
    int16_t temperature_alert_threshold_c10;
    uint8_t active_device;      // current device, independent of browsing cursor
    uint8_t selected;           // current selection in menu
    uint8_t item_count;         // number of menu items
    const char *items[MENU_ITEM_MAX]; // menu item names
} mode_view_t;

typedef enum {
    MODE_ACTION_NONE = 0,
    MODE_ACTION_KEYBOARD_KEY,
    MODE_ACTION_KEYBOARD_PRESS,
    MODE_ACTION_KEYBOARD_RELEASE,
    MODE_ACTION_ABS_MOUSE_DRAG,
    MODE_ACTION_MEDIA,
    MODE_ACTION_DEVICE_SWITCH,
    MODE_ACTION_DISCONNECT,
    MODE_ACTION_PAIRING_MODE,
    MODE_ACTION_CLEAR_BONDS,
    MODE_ACTION_HID_MODE_TOGGLE,
    MODE_ACTION_GAME_TETRIS,
    MODE_ACTION_GAME_SHOOTER,
    MODE_ACTION_GAME_BREAKOUT,
    MODE_ACTION_GAME_SNAKE,
    MODE_ACTION_WEB_START,
    MODE_ACTION_WEB_STOP,
    MODE_ACTION_CUSTOM_SHORTCUT_TAP,
    MODE_ACTION_CUSTOM_SHORTCUT_PRESS,
    MODE_ACTION_CUSTOM_SHORTCUT_RELEASE,
} mode_action_type_t;

typedef struct {
    mode_action_type_t type;
    int16_t value;
    uint8_t modifier;
    button_key_t key;
} mode_action_t;

void mode_manager_init(void);
bool mode_manager_update(button_key_t key, uint32_t now_ms, mode_view_t *view, mode_action_t *action);
void mode_manager_enter_menu(mode_view_t *view);
bool mode_manager_is_douyin_pc_mode(void);
