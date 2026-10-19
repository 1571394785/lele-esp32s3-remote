#include "device_names.h"
#include "mode_manager.h"

#include <stdio.h>

#include "app_mouse.h"
#include "ble_hid.h"
#include "custom_mode.h"
#include "data_collector.h"
#include "gesture_inference.h"
#include "temperature_alert.h"
#include "nvs_flash.h"
#include "nvs.h"

#define NVS_NAMESPACE "remote"
#define NVS_KEY_MODE  "mode"

typedef enum {
    APP_MODE_DOUYIN_PC = 0,
    APP_MODE_DOUYIN_IOS,
    APP_MODE_DOUYIN_ANDROID,
    APP_MODE_MEDIA_CTRL,
    APP_MODE_CUSTOM,
    APP_MODE_GAMEPAD,
    APP_MODE_MOUSE,
} app_mode_t;

typedef enum {
    MENU_PAGE_ROOT = 0,
    MENU_PAGE_MODES,
    MENU_PAGE_DEVICES,
    MENU_PAGE_SETTINGS,
    MENU_PAGE_GAMES,
    MENU_PAGE_STATUS,
    MENU_PAGE_ABOUT,
    MENU_PAGE_WEB_CONTROL,
    MENU_PAGE_TEMPERATURE_ALERT_CONFIG,
    MENU_PAGE_HID_CONFIRM,
    MENU_PAGE_AIR_MOUSE_CONFIG,
    MENU_PAGE_AIR_WHEEL_CONFIG,
    MENU_PAGE_DATA_COLLECTION,
    MENU_PAGE_GESTURE_INFERENCE,
} menu_page_t;

static const char * const MODE_NAMES[MODE_COUNT] = {
    "抖音电脑版",
    "抖音IOS",
    "抖音安卓",
    "多媒体控制",
    "自定义模式",
    "手柄模式",
    "鼠标模式",
};

static const char * const MODE_ACTIONS[MODE_COUNT][4] = {
    [APP_MODE_DOUYIN_PC]     = {"方向上", "方向下", "待机", "右键锁定"},
    [APP_MODE_DOUYIN_IOS]    = {"上拖动", "下拖动", "待机", "空"},
    [APP_MODE_DOUYIN_ANDROID]= {"上拖动", "下拖动", "待机", "空"},
    [APP_MODE_MEDIA_CTRL]    = {"音量+", "音量-", "待机", "媒体键"},
    [APP_MODE_CUSTOM]        = {"自定义", "自定义", "待机", "自定义"},
    [APP_MODE_GAMEPAD]       = {"十字上", "十字下", "待机", "ABXY"},
    [APP_MODE_MOUSE]         = {"向上移动", "向下移动", "待机", "左右移动"},
};

static const char * const ROOT_MENU_ITEMS[] = {
    "模式切换",
    "设置",
    "游戏",
    "状态",
    "关于",
    "设备切换",
};

static char s_device_labels[BLE_HID_DEVICE_SLOT_COUNT][DEVICE_NAME_BYTES + 1];
static char s_device_title[DEVICE_NAME_BYTES + 24];

static const char * const GAME_MENU_ITEMS[] = {
    "俄罗斯方块",
    "雷霆战机",
    "弹砖块",
    "贪吃蛇",
};

static const char * const SETTINGS_MENU_ITEMS[] = {
    "断开连接",
    "配对模式",
    "恢复出厂设置",
    "网页控制",
    "温度提醒",
    "兼容模式",
    "飞鼠灵敏度",
    "滚轮灵敏度",
    "数据采集",
    "手势识别",
};

static app_mode_t s_mode;
static app_screen_t s_screen;
static menu_page_t s_menu_page;
static button_key_t s_last_key;
static uint8_t s_menu_sel;
static const char *s_last_action;
static bool s_pc_right_held;
static app_mode_t s_temporary_return_mode;
static bool s_temporary_mouse;
static char s_air_line1[24];
static char s_air_line2[24];
static char s_wheel_line1[24];
static char s_wheel_line2[24];
static char s_temperature_line1[24];
static char s_temperature_line2[24];
static char s_collect_line1[24];
static char s_collect_line2[24];
static char s_gesture_line1[24];
static char s_gesture_line2[24];
static uint32_t s_air_adjust_last_ms;

static void nvs_save_mode(uint8_t mode)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_MODE, mode);
        nvs_commit(h);
        nvs_close(h);
    }
}

static uint8_t nvs_load_mode(void)
{
    uint8_t mode = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY_MODE, &mode);
        nvs_close(h);
    }
    return mode;
}

static uint16_t step_u16(uint16_t value, int16_t delta, uint16_t min_value, uint16_t max_value)
{
    int32_t next = (int32_t)value + delta;
    if (next < min_value) {
        return min_value;
    }
    if (next > max_value) {
        return max_value;
    }
    return (uint16_t)next;
}

static const char *action_text(app_mode_t mode, button_key_t key)
{
    int idx;
    switch (key) {
    case BUTTON_KEY_UP:    idx = 0; break;
    case BUTTON_KEY_DOWN:  idx = 1; break;
    case BUTTON_KEY_NONE:  idx = 2; break;
    case BUTTON_KEY_LEFT:
    case BUTTON_KEY_RIGHT: idx = 3; break;
    default:               idx = 3; break;
    }
    return MODE_ACTIONS[mode][idx];
}

static mode_action_t make_action(app_mode_t mode, button_key_t key)
{
    mode_action_t a = {MODE_ACTION_NONE, 0, 0, BUTTON_KEY_NONE};

    if (key == BUTTON_KEY_NONE) {
        return a;
    }

    switch (mode) {
    case APP_MODE_DOUYIN_PC:
        if (s_pc_right_held && key != BUTTON_KEY_RIGHT) {
            s_pc_right_held = false;
        }
        if (key == BUTTON_KEY_UP) {
            a.type = MODE_ACTION_KEYBOARD_KEY;
            a.value = 0x52;
        } else if (key == BUTTON_KEY_DOWN) {
            a.type = MODE_ACTION_KEYBOARD_KEY;
            a.value = 0x51;
        } else if (key == BUTTON_KEY_LEFT) {
            a.type = MODE_ACTION_KEYBOARD_KEY;
            a.value = 0x1B; // Keyboard X
        } else if (key == BUTTON_KEY_FUNC2) {
            a.type = MODE_ACTION_KEYBOARD_KEY;
            a.value = 0x0B; // Keyboard H
        } else if (key == BUTTON_KEY_FUNC3) {
            a.type = MODE_ACTION_KEYBOARD_KEY;
            a.value = 0x2C; // Keyboard Space
        } else if (key == BUTTON_KEY_RIGHT) {
            s_pc_right_held = !s_pc_right_held;
            a.type = s_pc_right_held ? MODE_ACTION_KEYBOARD_PRESS : MODE_ACTION_KEYBOARD_RELEASE;
            a.value = 0x4F; // Keyboard Right Arrow
        }
        break;

    case APP_MODE_DOUYIN_IOS:
        if (key == BUTTON_KEY_UP) {
            a.type = MODE_ACTION_ABS_MOUSE_DRAG; a.value = 1;
        } else if (key == BUTTON_KEY_DOWN) {
            a.type = MODE_ACTION_ABS_MOUSE_DRAG; a.value = -1;
        }
        break;

    case APP_MODE_DOUYIN_ANDROID:
        if (key == BUTTON_KEY_UP) {
            a.type = MODE_ACTION_ABS_MOUSE_DRAG; a.value = -1;
        } else if (key == BUTTON_KEY_DOWN) {
            a.type = MODE_ACTION_ABS_MOUSE_DRAG; a.value = 1;
        }
        break;

    case APP_MODE_MEDIA_CTRL:
        if (key == BUTTON_KEY_UP) {
            a.type = MODE_ACTION_MEDIA; a.value = 0x00E9; // Volume Increment
        } else if (key == BUTTON_KEY_DOWN) {
            a.type = MODE_ACTION_MEDIA; a.value = 0x00EA; // Volume Decrement
        } else if (key == BUTTON_KEY_LEFT) {
            a.type = MODE_ACTION_MEDIA; a.value = 0x00B6; // Scan Previous Track
        } else if (key == BUTTON_KEY_RIGHT) {
            a.type = MODE_ACTION_MEDIA; a.value = 0x00B5; // Scan Next Track
        } else if (key == BUTTON_KEY_FUNC2) {
            a.type = MODE_ACTION_MEDIA; a.value = 0x00CD; // Play/Pause
        } else if (key == BUTTON_KEY_FUNC3) {
            a.type = MODE_ACTION_MEDIA; a.value = 0x00E2; // Mute
        }
        break;

    case APP_MODE_CUSTOM: {
        a.type = MODE_ACTION_CUSTOM_SHORTCUT_PRESS;
        a.key = key;
        break;
    }

    case APP_MODE_GAMEPAD:
    case APP_MODE_MOUSE:
        break;

    default:
        break;
    }
    return a;
}

static void fill_view(mode_view_t *view)
{
    if (view == NULL) return;

    if (s_screen == APP_SCREEN_MENU) {
        view->screen = APP_SCREEN_MENU;
        view->line1 = NULL;
        view->line2 = NULL;
        view->show_addr = false;
        view->show_status = s_menu_page == MENU_PAGE_STATUS;
        view->show_web_control = s_menu_page == MENU_PAGE_WEB_CONTROL;
        view->show_temperature_alert_config = s_menu_page == MENU_PAGE_TEMPERATURE_ALERT_CONFIG;
        view->show_about = s_menu_page == MENU_PAGE_ABOUT;
        view->show_hid_confirm = s_menu_page == MENU_PAGE_HID_CONFIRM;
        view->show_air_mouse_config = s_menu_page == MENU_PAGE_AIR_MOUSE_CONFIG;
        view->show_air_wheel_config = s_menu_page == MENU_PAGE_AIR_WHEEL_CONFIG;
        view->show_data_collection = s_menu_page == MENU_PAGE_DATA_COLLECTION;
        view->show_gesture_inference = s_menu_page == MENU_PAGE_GESTURE_INFERENCE;
        view->show_devices = s_menu_page == MENU_PAGE_DEVICES;
        view->blink_selected = false;
        view->is_gamepad_mode = false;
        view->is_mouse_mode = false;
        view->is_temporary_mouse = false;
        view->show_scrollbar = false;
        view->selected = s_menu_sel;

        switch (s_menu_page) {
        case MENU_PAGE_ROOT:
            view->title = "菜单";
            view->item_count = sizeof(ROOT_MENU_ITEMS) / sizeof(ROOT_MENU_ITEMS[0]);
            view->show_scrollbar = view->item_count > 3;
            for (uint8_t i = 0; i < view->item_count; i++) {
                view->items[i] = ROOT_MENU_ITEMS[i];
            }
            break;

        case MENU_PAGE_DEVICES:
            view->blink_selected = !ble_hid_is_connected();
            view->active_device = ble_hid_get_selected_device_slot();
            view->item_count = BLE_HID_DEVICE_SLOT_COUNT;
            for (uint8_t i = 0; i < BLE_HID_DEVICE_SLOT_COUNT; ++i) {
                device_names_get(i, s_device_labels[i], sizeof(s_device_labels[i]));
                view->items[i] = s_device_labels[i];
            }
            snprintf(s_device_title, sizeof(s_device_title), "当前设备:%s", s_device_labels[view->active_device]);
            view->title = s_device_title;
            break;

        case MENU_PAGE_MODES:
            view->title = "模式切换";
            view->item_count = MODE_COUNT;
            view->show_scrollbar = view->item_count > 3;
            for (uint8_t i = 0; i < view->item_count; i++) {
                view->items[i] = MODE_NAMES[i];
            }
            break;

        case MENU_PAGE_GAMES:
            view->title = "游戏";
            view->item_count = sizeof(GAME_MENU_ITEMS) / sizeof(GAME_MENU_ITEMS[0]);
            view->show_scrollbar = view->item_count > 3;
            for (uint8_t i = 0; i < view->item_count; i++) {
                view->items[i] = GAME_MENU_ITEMS[i];
            }
            break;

        case MENU_PAGE_SETTINGS:
            view->title = "设置";
            view->item_count = sizeof(SETTINGS_MENU_ITEMS) / sizeof(SETTINGS_MENU_ITEMS[0]);
            view->show_scrollbar = view->item_count > 3;
            for (uint8_t i = 0; i < view->item_count; i++) {
                if (i == 4) {
                    view->items[i] = temperature_alert_is_enabled() ?
                                     "温度提醒 开" : "温度提醒 关";
                } else if (i == 5) {
                    view->items[i] = ble_hid_is_full_mode() ?
                                     "当前是完全模式" : "当前是兼容模式";
                } else {
                    view->items[i] = SETTINGS_MENU_ITEMS[i];
                }
            }
            break;

        case MENU_PAGE_STATUS:
            view->title = "状态";
            view->item_count = 0;
            break;

        case MENU_PAGE_ABOUT:
            view->title = "关于";
            view->item_count = 0;
            break;

        case MENU_PAGE_WEB_CONTROL:
            view->title = "网页控制";
            view->item_count = 0;
            break;

        case MENU_PAGE_TEMPERATURE_ALERT_CONFIG: {
            int16_t threshold = temperature_alert_get_threshold_c10();
            view->title = "温度提醒";
            snprintf(s_temperature_line1, sizeof(s_temperature_line1), "提醒 %s",
                     temperature_alert_is_temporarily_disabled() ? "临时关" :
                     temperature_alert_is_enabled() ? "开启" : "关闭");
            snprintf(s_temperature_line2, sizeof(s_temperature_line2), "阈值 %d.%dC",
                     threshold / 10, threshold % 10);
            view->line1 = s_temperature_line1;
            view->line2 = s_temperature_line2;
            view->temperature_alert_enabled = temperature_alert_is_enabled();
            view->temperature_alert_temporarily_disabled =
                temperature_alert_is_temporarily_disabled();
            view->temperature_alert_threshold_c10 = threshold;
            view->item_count = 0;
            break;
        }

        case MENU_PAGE_HID_CONFIRM:
            view->title = ble_hid_is_full_mode() ? "切到兼容模式" : "切到完全模式";
            view->line1 = "切换会重启";
            view->line2 = "F1确认 F2取消";
            view->item_count = 0;
            break;

        case MENU_PAGE_AIR_MOUSE_CONFIG:
            view->title = "飞鼠灵敏度";
            snprintf(s_air_line1, sizeof(s_air_line1), "X%u Y%u",
                     app_mouse_get_air_x_sensitivity(),
                     app_mouse_get_air_y_sensitivity());
            snprintf(s_air_line2, sizeof(s_air_line2), "A%u F3- F4+",
                     app_mouse_get_air_accel());
            view->line1 = s_air_line1;
            view->line2 = s_air_line2;
            view->item_count = 0;
            break;

        case MENU_PAGE_AIR_WHEEL_CONFIG:
            view->title = "滚轮灵敏度";
            snprintf(s_wheel_line1, sizeof(s_wheel_line1), "Wheel %u",
                     app_mouse_get_air_wheel_sensitivity());
            snprintf(s_wheel_line2, sizeof(s_wheel_line2), "Up/Down +/-5");
            view->line1 = s_wheel_line1;
            view->line2 = s_wheel_line2;
            view->item_count = 0;
            break;

        case MENU_PAGE_DATA_COLLECTION:
            view->title = "数据采集";
            snprintf(s_collect_line1, sizeof(s_collect_line1), "%s %ums",
                     data_collector_is_running() ? "Running" : "Stopped",
                     (unsigned)data_collector_sample_period_ms());
            snprintf(s_collect_line2, sizeof(s_collect_line2), "Label %s",
                     data_collector_label());
            view->line1 = s_collect_line1;
            view->line2 = s_collect_line2;
            view->item_count = 0;
            break;

        case MENU_PAGE_GESTURE_INFERENCE:
            view->title = "手势识别";
            snprintf(s_gesture_line1, sizeof(s_gesture_line1), "%s W%d",
                     gesture_inference_is_running() ? "Running" : "Stopped",
                     gesture_inference_sample_count());
            snprintf(s_gesture_line2, sizeof(s_gesture_line2), "L%d P%.2f D%.1f",
                     gesture_inference_last_label(),
                     gesture_inference_last_probability(),
                     gesture_inference_last_distance());
            view->line1 = s_gesture_line1;
            view->line2 = s_gesture_line2;
            view->item_count = 0;
            break;
        }
        return;
    }

    view->screen = APP_SCREEN_MODE;
    view->title = MODE_NAMES[s_mode];
    if (s_mode == APP_MODE_DOUYIN_PC) {
        snprintf(s_gesture_line1, sizeof(s_gesture_line1), "G W%d L%d P%.2f",
                 gesture_inference_sample_count(),
                 gesture_inference_last_label(),
                 gesture_inference_last_probability());
        view->line1 = s_gesture_line1;
    } else {
        view->line1 = s_last_action;
    }
    view->line2 = (s_mode == APP_MODE_GAMEPAD) ? "BX菜单" :
                  (s_mode == APP_MODE_MOUSE && s_temporary_mouse) ? "F2返回" : "F1菜单";
    view->show_addr = false;
    view->show_status = false;
    view->show_web_control = false;
    view->show_temperature_alert_config = false;
    view->show_about = false;
    view->show_hid_confirm = false;
    view->show_air_mouse_config = false;
    view->show_air_wheel_config = false;
    view->show_data_collection = false;
    view->show_gesture_inference = false;
    view->show_devices = false;
    view->blink_selected = false;
    view->is_gamepad_mode = s_mode == APP_MODE_GAMEPAD;
    view->is_mouse_mode = s_mode == APP_MODE_MOUSE;
    view->is_temporary_mouse = s_mode == APP_MODE_MOUSE && s_temporary_mouse;
    view->show_scrollbar = false;
    view->temperature_alert_enabled = false;
    view->temperature_alert_temporarily_disabled = false;
    view->temperature_alert_threshold_c10 = 0;
    view->selected = 0;
    view->item_count = 0;
}

void mode_manager_init(void)
{
    uint8_t saved = nvs_load_mode();
    if (saved < MODE_COUNT) {
        s_mode = (app_mode_t)saved;
    } else {
        s_mode = APP_MODE_DOUYIN_PC;
    }
    s_screen = APP_SCREEN_MODE;
    s_menu_page = MENU_PAGE_ROOT;
    s_last_key = BUTTON_KEY_NONE;
    s_menu_sel = 0;
    s_last_action = "待机";
    s_pc_right_held = false;
    s_temporary_return_mode = APP_MODE_DOUYIN_PC;
    s_temporary_mouse = false;
    s_air_adjust_last_ms = 0;
}

void mode_manager_enter_menu(mode_view_t *view)
{
    s_menu_page = MENU_PAGE_ROOT;
    s_menu_sel = 0;
    s_screen = APP_SCREEN_MENU;
    s_last_key = BUTTON_KEY_NONE;
    fill_view(view);
}

bool mode_manager_is_douyin_pc_mode(void)
{
    return s_screen == APP_SCREEN_MODE && s_mode == APP_MODE_DOUYIN_PC;
}

bool mode_manager_update(button_key_t key, uint32_t now_ms, mode_view_t *view, mode_action_t *action)
{
    bool changed = key != s_last_key;

    if (action != NULL) {
        action->type = MODE_ACTION_NONE;
        action->value = 0;
        action->modifier = 0;
        action->key = BUTTON_KEY_NONE;
    }

    /* ── Menu mode ── */
    if (s_screen == APP_SCREEN_MENU) {
        if (s_menu_page == MENU_PAGE_TEMPERATURE_ALERT_CONFIG) {
            if (key != s_last_key) {
                if (key == BUTTON_KEY_FUNC1) {
                    if (temperature_alert_is_temporarily_disabled()) {
                        temperature_alert_clear_temporary_disable();
                    } else {
                        (void)temperature_alert_set_enabled(
                            !temperature_alert_is_configured_enabled());
                    }
                    changed = true;
                } else if (key == BUTTON_KEY_UP) {
                    (void)temperature_alert_set_threshold_c10(
                        temperature_alert_get_threshold_c10() + 10);
                    changed = true;
                } else if (key == BUTTON_KEY_DOWN) {
                    (void)temperature_alert_set_threshold_c10(
                        temperature_alert_get_threshold_c10() - 10);
                    changed = true;
                } else if (key == BUTTON_KEY_FUNC2) {
                    s_menu_page = MENU_PAGE_SETTINGS;
                    s_menu_sel = 4;
                    changed = true;
                }
            }
            s_last_key = key;
            fill_view(view);
            return changed;
        }

        if (s_menu_page == MENU_PAGE_AIR_MOUSE_CONFIG) {
            bool adjust_due = key != s_last_key || now_ms - s_air_adjust_last_ms >= 120;
            if (adjust_due) {
                if (key == BUTTON_KEY_LEFT) {
                    app_mouse_set_air_x_sensitivity(step_u16(app_mouse_get_air_x_sensitivity(), -5, 10, 500));
                    s_air_adjust_last_ms = now_ms;
                    changed = true;
                } else if (key == BUTTON_KEY_RIGHT) {
                    app_mouse_set_air_x_sensitivity(step_u16(app_mouse_get_air_x_sensitivity(), 5, 10, 500));
                    s_air_adjust_last_ms = now_ms;
                    changed = true;
                } else if (key == BUTTON_KEY_DOWN) {
                    app_mouse_set_air_y_sensitivity(step_u16(app_mouse_get_air_y_sensitivity(), -5, 10, 500));
                    s_air_adjust_last_ms = now_ms;
                    changed = true;
                } else if (key == BUTTON_KEY_UP) {
                    app_mouse_set_air_y_sensitivity(step_u16(app_mouse_get_air_y_sensitivity(), 5, 10, 500));
                    s_air_adjust_last_ms = now_ms;
                    changed = true;
                } else if (key == BUTTON_KEY_FUNC3) {
                    app_mouse_set_air_accel(step_u16(app_mouse_get_air_accel(), -10, 0, 500));
                    s_air_adjust_last_ms = now_ms;
                    changed = true;
                } else if (key == BUTTON_KEY_FUNC4) {
                    app_mouse_set_air_accel(step_u16(app_mouse_get_air_accel(), 10, 0, 500));
                    s_air_adjust_last_ms = now_ms;
                    changed = true;
                } else if (key == BUTTON_KEY_FUNC2) {
                    s_menu_page = MENU_PAGE_SETTINGS;
                    s_menu_sel = 6;
                    changed = true;
                }
            }
            s_last_key = key;
            fill_view(view);
            return changed;
        }

        if (s_menu_page == MENU_PAGE_AIR_WHEEL_CONFIG) {
            bool adjust_due = key != s_last_key || now_ms - s_air_adjust_last_ms >= 120;
            if (adjust_due) {
                if (key == BUTTON_KEY_DOWN || key == BUTTON_KEY_LEFT) {
                    app_mouse_set_air_wheel_sensitivity(
                        step_u16(app_mouse_get_air_wheel_sensitivity(), -5, 10, 500));
                    s_air_adjust_last_ms = now_ms;
                    changed = true;
                } else if (key == BUTTON_KEY_UP || key == BUTTON_KEY_RIGHT) {
                    app_mouse_set_air_wheel_sensitivity(
                        step_u16(app_mouse_get_air_wheel_sensitivity(), 5, 10, 500));
                    s_air_adjust_last_ms = now_ms;
                    changed = true;
                } else if (key == BUTTON_KEY_FUNC2) {
                    s_menu_page = MENU_PAGE_SETTINGS;
                    s_menu_sel = 7;
                    changed = true;
                }
            }
            s_last_key = key;
            fill_view(view);
            return changed;
        }

        if (s_menu_page == MENU_PAGE_DATA_COLLECTION) {
            if (key == BUTTON_KEY_FUNC1 && key != s_last_key) {
                data_collector_toggle();
                changed = true;
            } else if (key == BUTTON_KEY_FUNC2 && key != s_last_key) {
                s_menu_page = MENU_PAGE_SETTINGS;
                s_menu_sel = 8;
                changed = true;
            }
            s_last_key = key;
            fill_view(view);
            return changed;
        }

        if (s_menu_page == MENU_PAGE_GESTURE_INFERENCE) {
            if (key == BUTTON_KEY_FUNC1 && key != s_last_key) {
                gesture_inference_toggle();
                changed = true;
            } else if (key == BUTTON_KEY_FUNC2 && key != s_last_key) {
                s_menu_page = MENU_PAGE_SETTINGS;
                s_menu_sel = 9;
                changed = true;
            }
            s_last_key = key;
            fill_view(view);
            return changed;
        }

        if (key == BUTTON_KEY_FUNC1 && key != s_last_key) {
            if (s_menu_page == MENU_PAGE_ROOT) {
                if (s_menu_sel == 0) {
                    s_menu_page = MENU_PAGE_MODES;
                } else if (s_menu_sel == 1) {
                    s_menu_page = MENU_PAGE_SETTINGS;
                } else if (s_menu_sel == 2) {
                    s_menu_page = MENU_PAGE_GAMES;
                } else if (s_menu_sel == 3) {
                    s_menu_page = MENU_PAGE_STATUS;
                } else if (s_menu_sel == 4) {
                    s_menu_page = MENU_PAGE_ABOUT;
                } else {
                    s_menu_page = MENU_PAGE_DEVICES;
                }
                s_menu_sel = (s_menu_page == MENU_PAGE_MODES) ? (uint8_t)s_mode :
                             (s_menu_page == MENU_PAGE_DEVICES) ? ble_hid_get_selected_device_slot() : 0;
            } else if (s_menu_page == MENU_PAGE_DEVICES) {
                if (action != NULL) {
                    action->type = MODE_ACTION_DEVICE_SWITCH;
                    action->value = s_menu_sel;
                }
            } else if (s_menu_page == MENU_PAGE_MODES) {
                s_mode = (app_mode_t)s_menu_sel;
                s_temporary_mouse = false;
                nvs_save_mode((uint8_t)s_mode);
                s_screen = APP_SCREEN_MODE;
                s_menu_page = MENU_PAGE_ROOT;
                s_last_action = "待机";
            } else if (s_menu_page == MENU_PAGE_GAMES) {
                if (action != NULL) {
                    if (s_menu_sel == 0) {
                        action->type = MODE_ACTION_GAME_TETRIS;
                    } else if (s_menu_sel == 1) {
                        action->type = MODE_ACTION_GAME_SHOOTER;
                    } else if (s_menu_sel == 2) {
                        action->type = MODE_ACTION_GAME_BREAKOUT;
                    } else {
                        action->type = MODE_ACTION_GAME_SNAKE;
                    }
                }
                s_screen = APP_SCREEN_MODE;
                s_menu_page = MENU_PAGE_ROOT;
                s_last_action = GAME_MENU_ITEMS[s_menu_sel];
            } else if (s_menu_page == MENU_PAGE_SETTINGS) {
                if (action != NULL) {
                    if (s_menu_sel == 0) {
                        action->type = MODE_ACTION_DISCONNECT;
                    } else if (s_menu_sel == 1) {
                        action->type = MODE_ACTION_PAIRING_MODE;
                    } else if (s_menu_sel == 2) {
                        action->type = MODE_ACTION_CLEAR_BONDS;
                    } else if (s_menu_sel == 3) {
                        action->type = MODE_ACTION_WEB_START;
                    }
                }
                if (s_menu_sel == 0) {
                    s_screen = APP_SCREEN_MODE;
                    s_menu_page = MENU_PAGE_ROOT;
                    s_last_action = "已断开";
                } else if (s_menu_sel == 1) {
                    s_screen = APP_SCREEN_MODE;
                    s_menu_page = MENU_PAGE_ROOT;
                    s_last_action = "配对模式";
                } else if (s_menu_sel == 2) {
                    s_screen = APP_SCREEN_MODE;
                    s_menu_page = MENU_PAGE_ROOT;
                    s_mode = APP_MODE_DOUYIN_PC;
                    nvs_save_mode((uint8_t)s_mode);
                    app_mouse_set_air_x_sensitivity(100);
                    app_mouse_set_air_y_sensitivity(100);
                    app_mouse_set_air_accel(0);
                    app_mouse_set_air_wheel_sensitivity(100);
                    s_last_action = "恢复出厂";
                } else if (s_menu_sel == 3) {
                    s_menu_page = MENU_PAGE_WEB_CONTROL;
                    s_menu_sel = 0;
                } else if (s_menu_sel == 4) {
                    s_menu_page = MENU_PAGE_TEMPERATURE_ALERT_CONFIG;
                    s_menu_sel = 0;
                } else if (s_menu_sel == 5) {
                    s_menu_page = MENU_PAGE_HID_CONFIRM;
                    s_menu_sel = 0;
                } else if (s_menu_sel == 6) {
                    s_menu_page = MENU_PAGE_AIR_MOUSE_CONFIG;
                    s_menu_sel = 0;
                } else if (s_menu_sel == 7) {
                    s_menu_page = MENU_PAGE_AIR_WHEEL_CONFIG;
                    s_menu_sel = 0;
                } else if (s_menu_sel == 8) {
                    s_menu_page = MENU_PAGE_DATA_COLLECTION;
                    s_menu_sel = 0;
                } else if (s_menu_sel == 9) {
                    s_menu_page = MENU_PAGE_GESTURE_INFERENCE;
                    s_menu_sel = 0;
                }
            } else if (s_menu_page == MENU_PAGE_STATUS) {
                s_menu_sel = 0;
            } else if (s_menu_page == MENU_PAGE_ABOUT) {
                s_menu_sel = 0;
            } else if (s_menu_page == MENU_PAGE_WEB_CONTROL) {
                s_menu_sel = 0;
            } else if (s_menu_page == MENU_PAGE_HID_CONFIRM) {
                if (action != NULL) {
                    action->type = MODE_ACTION_HID_MODE_TOGGLE;
                }
                s_screen = APP_SCREEN_MODE;
                s_menu_page = MENU_PAGE_ROOT;
                s_last_action = ble_hid_is_full_mode() ? "兼容模式" : "完全模式";
            }
            changed = true;
        } else if (key == BUTTON_KEY_FUNC2 && key != s_last_key) {
            if (s_menu_page == MENU_PAGE_ROOT) {
                s_screen = APP_SCREEN_MODE;
            } else if (s_menu_page == MENU_PAGE_WEB_CONTROL) {
                if (action != NULL) {
                    action->type = MODE_ACTION_WEB_STOP;
                }
                s_menu_page = MENU_PAGE_ROOT;
                s_menu_sel = 0;
            } else if (s_menu_page == MENU_PAGE_HID_CONFIRM) {
                s_menu_page = MENU_PAGE_SETTINGS;
                s_menu_sel = 5;
            } else if (s_menu_page == MENU_PAGE_AIR_MOUSE_CONFIG) {
                s_menu_page = MENU_PAGE_SETTINGS;
                s_menu_sel = 6;
            } else if (s_menu_page == MENU_PAGE_AIR_WHEEL_CONFIG) {
                s_menu_page = MENU_PAGE_SETTINGS;
                s_menu_sel = 7;
            } else if (s_menu_page == MENU_PAGE_DATA_COLLECTION) {
                s_menu_page = MENU_PAGE_SETTINGS;
                s_menu_sel = 8;
            } else if (s_menu_page == MENU_PAGE_GESTURE_INFERENCE) {
                s_menu_page = MENU_PAGE_SETTINGS;
                s_menu_sel = 9;
            } else {
                s_menu_page = MENU_PAGE_ROOT;
                s_menu_sel = 0;
            }
            changed = true;
        } else if (key == BUTTON_KEY_UP && key != s_last_key) {
            if (s_menu_sel > 0) s_menu_sel--;
            changed = true;
        } else if (key == BUTTON_KEY_DOWN && key != s_last_key) {
            uint8_t max_sel = 0;
            if (s_menu_page == MENU_PAGE_ROOT) {
                max_sel = (sizeof(ROOT_MENU_ITEMS) / sizeof(ROOT_MENU_ITEMS[0])) - 1;
            } else if (s_menu_page == MENU_PAGE_DEVICES) {
                max_sel = BLE_HID_DEVICE_SLOT_COUNT - 1;
            } else if (s_menu_page == MENU_PAGE_MODES) {
                max_sel = MODE_COUNT - 1;
            } else if (s_menu_page == MENU_PAGE_GAMES) {
                max_sel = (sizeof(GAME_MENU_ITEMS) / sizeof(GAME_MENU_ITEMS[0])) - 1;
            } else if (s_menu_page == MENU_PAGE_SETTINGS) {
                max_sel = (sizeof(SETTINGS_MENU_ITEMS) / sizeof(SETTINGS_MENU_ITEMS[0])) - 1;
            } else if (s_menu_page == MENU_PAGE_STATUS) {
                max_sel = 0;
            } else if (s_menu_page == MENU_PAGE_ABOUT) {
                max_sel = 0;
            } else if (s_menu_page == MENU_PAGE_WEB_CONTROL) {
                max_sel = 0;
            } else if (s_menu_page == MENU_PAGE_TEMPERATURE_ALERT_CONFIG) {
                max_sel = 0;
            } else if (s_menu_page == MENU_PAGE_HID_CONFIRM) {
                max_sel = 0;
            } else if (s_menu_page == MENU_PAGE_AIR_MOUSE_CONFIG) {
                max_sel = 0;
            } else if (s_menu_page == MENU_PAGE_AIR_WHEEL_CONFIG) {
                max_sel = 0;
            } else if (s_menu_page == MENU_PAGE_DATA_COLLECTION) {
                max_sel = 0;
            } else if (s_menu_page == MENU_PAGE_GESTURE_INFERENCE) {
                max_sel = 0;
            }
            if (s_menu_sel < max_sel) s_menu_sel++;
            changed = true;
        }
        s_last_key = key;
        fill_view(view);
        return changed;
    }

    /* ── Mode screen ── */
    if (s_mode == APP_MODE_MOUSE && s_temporary_mouse &&
        key == BUTTON_KEY_FUNC2 && key != s_last_key) {
        s_mode = s_temporary_return_mode;
        s_temporary_mouse = false;
        s_last_action = "待机";
        changed = true;
        s_last_key = key;
        fill_view(view);
        return changed;
    }

    if (s_mode == APP_MODE_DOUYIN_PC && key == BUTTON_KEY_FUNC4 && key != s_last_key) {
        if (s_pc_right_held && action != NULL) {
            action->type = MODE_ACTION_KEYBOARD_RELEASE;
            action->value = 0x4F;
            s_pc_right_held = false;
        }
        s_temporary_return_mode = s_mode;
        s_temporary_mouse = true;
        s_mode = APP_MODE_MOUSE;
        s_last_action = "临时鼠标";
        changed = true;
        s_last_key = key;
        fill_view(view);
        return changed;
    }

    // FUNC1 enters menu in normal modes. Gamepad mode uses FUNC1+FUNC2 long press.
    if (s_mode != APP_MODE_GAMEPAD && key == BUTTON_KEY_FUNC1 && key != s_last_key) {
        if (s_pc_right_held && action != NULL) {
            action->type = MODE_ACTION_KEYBOARD_RELEASE;
            action->value = 0x4F;
            s_pc_right_held = false;
        }
        if (s_mode == APP_MODE_MOUSE && s_temporary_mouse) {
            s_mode = s_temporary_return_mode;
            s_temporary_mouse = false;
        }
        mode_manager_enter_menu(view);
        changed = true;
        s_last_key = key;
        return changed;
    }

    // Direction keys → trigger action
    if (s_mode == APP_MODE_CUSTOM && key == BUTTON_KEY_NONE &&
        s_last_key != BUTTON_KEY_NONE && s_last_key != BUTTON_KEY_FUNC1) {
        if (action != NULL) {
            action->type = MODE_ACTION_CUSTOM_SHORTCUT_RELEASE;
            action->key = s_last_key;
        }
        changed = true;
    } else if (key != BUTTON_KEY_FUNC1 && key != s_last_key) {
        s_last_action = action_text(s_mode, key);
        if (action != NULL) {
            *action = make_action(s_mode, key);
        }
        if (s_mode == APP_MODE_DOUYIN_PC && key == BUTTON_KEY_RIGHT) {
            s_last_action = s_pc_right_held ? "右键已锁定" : "右键已解除";
        } else if (s_mode == APP_MODE_DOUYIN_PC && key == BUTTON_KEY_LEFT) {
            s_last_action = "X";
        } else if (s_mode == APP_MODE_DOUYIN_PC && key == BUTTON_KEY_FUNC2) {
            s_last_action = "H";
        } else if (s_mode == APP_MODE_DOUYIN_PC && key == BUTTON_KEY_FUNC3) {
            s_last_action = "空格";
        } else if (s_mode == APP_MODE_MOUSE && key == BUTTON_KEY_FUNC4) {
            s_last_action = "左键";
        } else if (s_mode == APP_MODE_MOUSE && key == BUTTON_KEY_FUNC3) {
            s_last_action = "右键";
        }
    }

    s_last_key = key;
    fill_view(view);
    return changed;
}
