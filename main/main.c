#include "device_names.h"
#include "esp_err.h"
#include "esp_log.h"
#include "app_display.h"
#include "app_games.h"
#include "app_gamepad.h"
#include "app_mouse.h"
#include "attitude_history.h"
#include "battery_adc.h"
#include "bmi270.h"
#include "ble_hid.h"
#include "buttons.h"
#include "custom_mode.h"
#include "data_collector.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gesture_inference.h"
#include "mode_manager.h"
#include "ssd1306.h"
#include "temperature_alert.h"
#include "web_control.h"

static const char *TAG = "app";

#define GIF_FRAME_INTERVAL_MS 30
#define TEMP_ALERT_BLINK_MS 250

static const char *custom_http_state_text(custom_http_state_t state)
{
    switch (state) {
    case CUSTOM_HTTP_STATE_STARTING_WIFI: return "启动 WiFi";
    case CUSTOM_HTTP_STATE_CONNECTING_WIFI: return "连接 WiFi";
    case CUSTOM_HTTP_STATE_SENDING: return "发送请求";
    case CUSTOM_HTTP_STATE_DISCONNECTING: return "断开 WiFi";
    case CUSTOM_HTTP_STATE_SUCCESS: return "请求成功";
    case CUSTOM_HTTP_STATE_FAILED: return "请求失败";
    default: return "";
    }
}

static bool gesture_to_key(uint32_t now_ms, button_key_t *key)
{
    (void)now_ms;

    if (key == NULL || !gesture_inference_is_running()) {
        return false;
    }

    int label = gesture_inference_last_label();
    float probability = gesture_inference_last_probability();
    if (label == 1 &&
        probability >= GESTURE_INFERENCE_ACTION_THRESHOLD) {
        *key = BUTTON_KEY_DOWN;
        ESP_LOGI(TAG, "gesture %d %.2f dist %.2f -> DOWN",
                 label, probability, gesture_inference_last_distance());
        gesture_inference_reset_window();
        return true;
    }

    return false;
}

void app_main(void)
{
    // 开机延时 1000ms，让电源电压稳定后再初始化外设
    vTaskDelay(pdMS_TO_TICKS(500));

    esp_err_t ret = ssd1306_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OLED init failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_ERROR_CHECK(app_display_show_boot());
    vTaskDelay(pdMS_TO_TICKS(1500));

    ret = battery_adc_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Battery ADC init failed: %s", esp_err_to_name(ret));
    }

    ret = bmi270_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "BMI220 init failed: %s", esp_err_to_name(ret));
    }
    // Start periodic GPIO sampling after the sensor reset and firmware upload.
    ret = buttons_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Buttons init failed: %s", esp_err_to_name(ret));
        return;
    }

    attitude_history_init();

    ret = ble_hid_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BLE HID init failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = custom_mode_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Custom mode init failed: %s", esp_err_to_name(ret));
    }
    ret = temperature_alert_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Temperature alert config init failed: %s", esp_err_to_name(ret));
    }
    app_mouse_config_init();
    ESP_ERROR_CHECK(data_collector_init());
    ESP_ERROR_CHECK(gesture_inference_init());

    mode_manager_init();

    mode_view_t view = {0};
    mode_action_t action = {0};
    mode_manager_update(BUTTON_KEY_NONE, 0, &view, &action);
    ESP_ERROR_CHECK(app_display_render_view(&view, false, NULL, 0, NULL, NULL));
    gesture_inference_start();

    ESP_LOGI(TAG, "Mode manager ready");

    bool prev_connected = false;
    uint32_t last_status_refresh_ms = 0;
    uint32_t last_header_refresh_ms = 0;
    bool menu_func1_down = false;
    bool menu_func1_was_long = false;
    bool gif_playing = false;
    uint16_t gif_frame = 0;
    uint32_t last_gif_frame_ms = 0;
    app_gamepad_state_t gamepad = {0};
    app_gamepad_state_init(&gamepad);
    app_game_state_t game_state = {0};
    app_games_init(&game_state);
    app_mouse_state_t mouse = {0};
    app_mouse_state_init(&mouse);
    bool temp_alert_active = false;
    bool temp_alert_visible = false;
    bool temp_alert_disable_wait_release = false;
    uint32_t last_temp_alert_blink_ms = 0;
    custom_http_state_t last_custom_http_state = CUSTOM_HTTP_STATE_IDLE;

    while (true) {
        ble_hid_poll();
        uint16_t button_mask = buttons_poll_mask();
        button_key_t key = buttons_first_key_from_mask(button_mask);
        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        bool connected = ble_hid_is_connected();
        const char *addr = ble_hid_get_connected_addr();
        bool manager_changed = false;
        (void)attitude_history_update(now_ms);

        attitude_sample_t latest_attitude = {0};
        bool have_latest_attitude = attitude_history_latest(&latest_attitude);
        bool over_temperature = temperature_alert_is_enabled() &&
                                have_latest_attitude &&
                                latest_attitude.imu.present &&
                                latest_attitude.imu.temp_c10 >
                                    temperature_alert_get_threshold_c10();
        if (over_temperature) {
            if (key == BUTTON_KEY_FUNC1) {
                temperature_alert_disable_temporarily();
                temp_alert_active = false;
                temp_alert_visible = false;
                temp_alert_disable_wait_release = true;
                last_temp_alert_blink_ms = now_ms;
                ESP_ERROR_CHECK(app_display_render_view(&view, connected, addr, now_ms,
                                                        &last_status_refresh_ms,
                                                        &last_header_refresh_ms));
                prev_connected = connected;
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            if (mouse.active) {
                ESP_ERROR_CHECK(app_mouse_leave(&mouse));
            }
            if (!temp_alert_active ||
                now_ms - last_temp_alert_blink_ms >= TEMP_ALERT_BLINK_MS) {
                temp_alert_active = true;
                temp_alert_visible = !temp_alert_visible;
                last_temp_alert_blink_ms = now_ms;
                ESP_ERROR_CHECK(app_display_render_temperature_alert(latest_attitude.imu.temp_c10,
                                                                    temp_alert_visible));
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        } else if (temp_alert_active) {
            temp_alert_active = false;
            temp_alert_visible = false;
            last_temp_alert_blink_ms = now_ms;
            ESP_ERROR_CHECK(app_display_render_view(&view, connected, addr, now_ms,
                                                    &last_status_refresh_ms,
                                                    &last_header_refresh_ms));
            prev_connected = connected;
        }

        if (temp_alert_disable_wait_release) {
            if (key == BUTTON_KEY_NONE) {
                temp_alert_disable_wait_release = false;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        custom_http_status_t custom_http_status;
        web_control_get_custom_http_status(&custom_http_status);
        if (custom_http_status.state != CUSTOM_HTTP_STATE_IDLE) {
            if (mouse.active) {
                ESP_ERROR_CHECK(app_mouse_leave(&mouse));
            }
            if (custom_http_status.state != last_custom_http_state) {
                ESP_ERROR_CHECK(app_display_render_network_progress(
                    custom_http_state_text(custom_http_status.state),
                    custom_http_status.detail));
                last_custom_http_state = custom_http_status.state;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        } else if (last_custom_http_state != CUSTOM_HTTP_STATE_IDLE) {
            last_custom_http_state = CUSTOM_HTTP_STATE_IDLE;
            ESP_ERROR_CHECK(app_display_render_view(&view, connected, addr, now_ms,
                                                    &last_status_refresh_ms,
                                                    &last_header_refresh_ms));
            prev_connected = connected;
        }

        if (mode_manager_is_douyin_pc_mode() && !gesture_inference_is_running()) {
            gesture_inference_start();
            manager_changed = true;
        }

        if (gif_playing) {
            if ((button_mask & BUTTON_MASK_FUNC2) != 0) {
                gif_playing = false;
                gif_frame = 0;
                mode_manager_enter_menu(&view);
                ESP_ERROR_CHECK(app_display_render_view(&view, connected, addr, now_ms,
                                                        &last_status_refresh_ms,
                                                        &last_header_refresh_ms));
                prev_connected = connected;
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            if (now_ms - last_gif_frame_ms >= GIF_FRAME_INTERVAL_MS) {
                ESP_ERROR_CHECK(app_display_render_gif_frame(gif_frame));
                gif_frame = (gif_frame + 1) % app_display_gif_frame_count();
                last_gif_frame_ms = now_ms;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (app_games_is_running(&game_state)) {
            bool exit_game = false;
            ESP_ERROR_CHECK(app_games_update(&game_state, button_mask, now_ms, &exit_game));
            if (exit_game) {
                mode_manager_enter_menu(&view);
                ESP_ERROR_CHECK(app_display_render_view(&view, connected, addr, now_ms,
                                                        &last_status_refresh_ms,
                                                        &last_header_refresh_ms));
                prev_connected = connected;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        action.type = MODE_ACTION_NONE;
        action.value = 0;
        action.modifier = 0;
        action.key = BUTTON_KEY_NONE;

        if (view.screen == APP_SCREEN_MODE && view.is_gamepad_mode) {
            ESP_ERROR_CHECK(app_gamepad_handle_mode(&gamepad, button_mask, now_ms,
                                                    &view, &manager_changed));
        } else {
            app_screen_t before_screen = view.screen;
            button_key_t input_key = key;
            app_gamepad_filter_menu_key(&gamepad, button_mask, &key);
            if (view.screen == APP_SCREEN_MENU) {
                bool func1_pressed = (button_mask & BUTTON_MASK_FUNC1) != 0;
                if (view.show_about && func1_pressed) {
                    if (!menu_func1_down) {
                        gif_playing = true;
                        gif_frame = 0;
                        last_gif_frame_ms = now_ms - GIF_FRAME_INTERVAL_MS;
                    }
                    menu_func1_down = true;
                    menu_func1_was_long = true;
                    key = BUTTON_KEY_NONE;
                } else if (func1_pressed) {
                    if (!menu_func1_down) {
                        menu_func1_down = true;
                        menu_func1_was_long = false;
                    }
                    key = BUTTON_KEY_NONE;
                } else if (menu_func1_down) {
                    key = menu_func1_was_long ? BUTTON_KEY_NONE : BUTTON_KEY_FUNC1;
                    menu_func1_down = false;
                    menu_func1_was_long = false;
                }
            } else {
                menu_func1_down = false;
                menu_func1_was_long = false;
            }
            if (view.screen == APP_SCREEN_MODE &&
                key == BUTTON_KEY_NONE &&
                button_mask == 0 &&
                mode_manager_is_douyin_pc_mode()) {
                (void)gesture_to_key(now_ms, &key);
            }
            manager_changed = mode_manager_update(key, now_ms, &view, &action);
            if (before_screen == APP_SCREEN_MODE && view.screen == APP_SCREEN_MENU &&
                input_key == BUTTON_KEY_FUNC1) {
                menu_func1_down = true;
                menu_func1_was_long = true;
            }
            app_gamepad_reset_if_inactive(&gamepad, &view);
        }

        if ((view.screen == APP_SCREEN_MODE && view.is_mouse_mode) || view.show_air_mouse_config) {
            if (!mouse.active) {
                app_mouse_enter(&mouse, button_mask);
            }
            uint16_t mouse_mask = button_mask;
            if (view.show_air_mouse_config) {
                mouse_mask &= BUTTON_MASK_UP | BUTTON_MASK_DOWN | BUTTON_MASK_LEFT | BUTTON_MASK_RIGHT;
            }
            ESP_ERROR_CHECK(app_mouse_handle(&mouse, mouse_mask, now_ms));
        } else if (mouse.active) {
            ESP_ERROR_CHECK(app_mouse_leave(&mouse));
        }

        bool status_refresh = view.screen == APP_SCREEN_MENU &&
                              ((view.show_devices && now_ms - last_status_refresh_ms >= 33) ||
                               (view.show_status && now_ms - last_status_refresh_ms >= 100));
        bool gesture_refresh = view.screen == APP_SCREEN_MENU && view.show_gesture_inference &&
                               now_ms - last_status_refresh_ms >= 250;
        bool header_refresh = view.screen == APP_SCREEN_MODE && now_ms - last_header_refresh_ms >= 1000;

        if (manager_changed || connected != prev_connected ||
            status_refresh || gesture_refresh || header_refresh) {
            ESP_ERROR_CHECK(app_display_render_view(&view, connected, addr, now_ms,
                                                    &last_status_refresh_ms,
                                                    &last_header_refresh_ms));

            if (!view.show_devices || manager_changed || connected != prev_connected)
                ESP_LOGI(TAG, "Screen: %s / %s / %s  conn=%s",
                     view.title, view.line1 ? view.line1 : "-",
                     view.line2 ? view.line2 : "-", addr ? addr : "none");
            prev_connected = connected;
        }
        if (action.type == MODE_ACTION_KEYBOARD_KEY) {
            ESP_ERROR_CHECK(ble_hid_send_key_combo(action.modifier, (uint8_t)action.value));
        } else if (action.type == MODE_ACTION_KEYBOARD_PRESS) {
            ESP_ERROR_CHECK(ble_hid_keyboard_press(action.modifier, (uint8_t)action.value));
        } else if (action.type == MODE_ACTION_KEYBOARD_RELEASE) {
            ESP_ERROR_CHECK(ble_hid_keyboard_release());
        } else if (action.type == MODE_ACTION_CUSTOM_SHORTCUT_TAP ||
                   action.type == MODE_ACTION_CUSTOM_SHORTCUT_PRESS) {
            if (custom_mode_get_action_type(action.key) == CUSTOM_ACTION_HTTP) {
                esp_err_t http_ret = web_control_start_custom_http(action.key);
                if (http_ret != ESP_OK) {
                    ESP_LOGW(TAG, "custom HTTP action rejected: %s", esp_err_to_name(http_ret));
                    ESP_ERROR_CHECK(app_display_render_network_progress("请求失败", "请检查网页配置"));
                }
            } else {
                custom_key_combo_t sequence[CUSTOM_SEQUENCE_MAX];
                size_t sequence_count = 0;
                if (custom_mode_get_sequence(action.key, sequence, CUSTOM_SEQUENCE_MAX, &sequence_count)) {
                    if (action.type == MODE_ACTION_CUSTOM_SHORTCUT_PRESS && sequence_count == 1) {
                        ESP_ERROR_CHECK(ble_hid_keyboard_press(sequence[0].modifier, sequence[0].keycode));
                    } else {
                        for (size_t i = 0; i < sequence_count; i++) {
                            ESP_ERROR_CHECK(ble_hid_send_key_combo(sequence[i].modifier, sequence[i].keycode));
                            vTaskDelay(pdMS_TO_TICKS(15));
                        }
                    }
                }
            }
        } else if (action.type == MODE_ACTION_CUSTOM_SHORTCUT_RELEASE) {
            if (custom_mode_get_action_type(action.key) == CUSTOM_ACTION_SHORTCUT) {
                ESP_ERROR_CHECK(ble_hid_keyboard_release());
            }
        } else if (action.type == MODE_ACTION_ABS_MOUSE_DRAG) {
            ESP_ERROR_CHECK(ble_hid_drag_vertical(action.value > 0));
        } else if (action.type == MODE_ACTION_MEDIA) {
            ESP_ERROR_CHECK(ble_hid_send_consumer((uint16_t)action.value));
        } else if (action.type == MODE_ACTION_DEVICE_SWITCH) {
            esp_err_t ret = ble_hid_switch_device_slot((uint8_t)action.value);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "device switch failed: %s", esp_err_to_name(ret));
                app_display_render_network_progress("切换失败", "请重试");
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        } else if (action.type == MODE_ACTION_DISCONNECT) {
            ESP_ERROR_CHECK(ble_hid_disconnect());
        } else if (action.type == MODE_ACTION_PAIRING_MODE) {
            ESP_ERROR_CHECK(ble_hid_enter_pairing_mode());
        } else if (action.type == MODE_ACTION_CLEAR_BONDS) {
            ESP_ERROR_CHECK(ble_hid_clear_pairing());
            ESP_ERROR_CHECK(device_names_reset());
            ESP_ERROR_CHECK(custom_mode_clear_all());
            ESP_ERROR_CHECK(temperature_alert_reset());
        } else if (action.type == MODE_ACTION_HID_MODE_TOGGLE) {
            ESP_ERROR_CHECK(ble_hid_toggle_hid_mode());
        } else if (action.type == MODE_ACTION_GAME_TETRIS) {
            app_games_start(&game_state, APP_GAME_TETRIS, now_ms);
        } else if (action.type == MODE_ACTION_GAME_SHOOTER) {
            app_games_start(&game_state, APP_GAME_SHOOTER, now_ms);
        } else if (action.type == MODE_ACTION_GAME_BREAKOUT) {
            app_games_start(&game_state, APP_GAME_BREAKOUT, now_ms);
        } else if (action.type == MODE_ACTION_GAME_SNAKE) {
            app_games_start(&game_state, APP_GAME_SNAKE, now_ms);
        } else if (action.type == MODE_ACTION_WEB_START) {
            ESP_ERROR_CHECK(web_control_start());
        } else if (action.type == MODE_ACTION_WEB_STOP) {
            ESP_ERROR_CHECK(web_control_stop());
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
