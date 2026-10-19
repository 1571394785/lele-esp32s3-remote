#include "device_names.h"
#include "app_display.h"

#include <stdio.h>
#include <string.h>

#include "battery_adc.h"
#include "bmi270.h"
#include "esp_check.h"
#include "gif_frames.h"
#include "qrcode_v2.h"
#include "ssd1306.h"
#include "temperature_alert.h"

static uint32_t s_device_scroll_step;
static bool s_device_scroll_active;
static char s_device_scroll_title[DEVICE_NAME_BYTES + 24];

static const char *TAG = "app_display";
static const char *ABOUT_QR_URL = "HTTP://39.104.86.15/";

uint8_t app_display_read_battery_percent(int *battery_mv)
{
    int mv = 0;
    if (battery_adc_read_actual_mv(&mv) != ESP_OK) {
        if (battery_mv != NULL) {
            *battery_mv = 0;
        }
        return 0;
    }

    if (battery_mv != NULL) {
        *battery_mv = mv;
    }
    return battery_adc_percent_from_mv(mv);
}

esp_err_t app_display_show_boot(void)
{
    ESP_RETURN_ON_ERROR(ssd1306_clear(), TAG, "clear boot screen");
    ESP_RETURN_ON_ERROR(ssd1306_draw_text24(4, 4, "万能遥控器"), TAG, "draw boot title");
    ESP_RETURN_ON_ERROR(ssd1306_draw_text16(5, 4, "Powered By 乐乐"), TAG, "draw boot subtitle");
    return ssd1306_flush();
}

static esp_err_t draw_marquee_rect(int x0, int y0, int w, int h, bool phase)
{
    const int segment = 8;
    const int thickness = 2;
    int idx = 0;
    int x1 = x0 + w - thickness;
    int y1 = y0 + h - thickness;

    for (int x = x0; x < x0 + w; x += segment) {
        int len = x + segment > x0 + w ? x0 + w - x : segment;
        bool on = ((idx + (phase ? 1 : 0)) & 1) == 0;
        ESP_RETURN_ON_ERROR(ssd1306_fill_rect(x, y0, len, thickness, on), TAG, "draw top alert border");
        idx++;
    }
    for (int y = y0 + segment; y < y0 + h; y += segment) {
        int len = y + segment > y0 + h ? y0 + h - y : segment;
        bool on = ((idx + (phase ? 1 : 0)) & 1) == 0;
        ESP_RETURN_ON_ERROR(ssd1306_fill_rect(x1, y, thickness, len, on),
                            TAG, "draw right alert border");
        idx++;
    }
    for (int x = x0 + w - segment; x >= x0; x -= segment) {
        int len = x + segment > x0 + w ? x0 + w - x : segment;
        bool on = ((idx + (phase ? 1 : 0)) & 1) == 0;
        ESP_RETURN_ON_ERROR(ssd1306_fill_rect(x, y1, len, thickness, on),
                            TAG, "draw bottom alert border");
        idx++;
    }
    for (int y = y0 + h - segment; y >= y0; y -= segment) {
        int len = y + segment > y0 + h ? y0 + h - y : segment;
        bool on = ((idx + (phase ? 1 : 0)) & 1) == 0;
        ESP_RETURN_ON_ERROR(ssd1306_fill_rect(x0, y, thickness, len, on), TAG, "draw left alert border");
        idx++;
    }

    return ESP_OK;
}

esp_err_t app_display_render_temperature_alert(int16_t temp_c10, bool phase)
{
    char temp_line[16];
    char voltage_line[16];
    int16_t whole = temp_c10 / 10;
    int16_t frac = temp_c10 % 10;
    int battery_mv = 0;
    if (frac < 0) {
        frac = (int16_t)-frac;
    }

    ESP_RETURN_ON_ERROR(ssd1306_clear(), TAG, "clear temp alert");
    ESP_RETURN_ON_ERROR(draw_marquee_rect(2, 18, 124, 44, phase), TAG, "draw temp alert border");
    snprintf(temp_line, sizeof(temp_line), "%d.%dC", whole, frac);
    if (battery_adc_read_actual_mv(&battery_mv) == ESP_OK && battery_mv > 0) {
        snprintf(voltage_line, sizeof(voltage_line), "%d.%02dV",
                 battery_mv / 1000, (battery_mv % 1000) / 10);
    } else {
        snprintf(voltage_line, sizeof(voltage_line), "--V");
    }
    ESP_RETURN_ON_ERROR(ssd1306_draw_text16(0, 32, "温度过高"), TAG, "draw temp alert title");
    if (phase) {
        ESP_RETURN_ON_ERROR(ssd1306_invert_area(0, 2, 0, OLED_WIDTH), TAG, "invert temp alert title");
    }
    ESP_RETURN_ON_ERROR(ssd1306_draw_text16(3, 8, temp_line), TAG, "draw temp alert value");
    ESP_RETURN_ON_ERROR(ssd1306_draw_text16(3, 80, voltage_line), TAG, "draw voltage value");
    ESP_RETURN_ON_ERROR(ssd1306_draw_text16(5, 8, "立刻移除充电器"), TAG, "draw temp alert hint");
    ESP_RETURN_ON_ERROR(ssd1306_draw_text(7, 32, "F1 Temp Off"),
                        TAG, "draw temp alert disable hint");
    return ssd1306_flush();
}

static esp_err_t draw_text16_wrapped(int start_page, int max_lines, const char *text)
{
    const char *p = text != NULL ? text : "";
    int line_index = 0;
    char line[64];
    size_t used = 0;
    int pixel_width = 0;

    while (*p != '\0' && line_index < max_lines) {
        unsigned char first = (unsigned char)*p;
        size_t char_len = 1;
        int char_width = 8;
        if ((first & 0xE0) == 0xC0) {
            char_len = 2;
            char_width = 16;
        } else if ((first & 0xF0) == 0xE0) {
            char_len = 3;
            char_width = 16;
        } else if ((first & 0xF8) == 0xF0) {
            char_len = 4;
            char_width = 16;
        }

        if ((pixel_width + char_width > OLED_WIDTH || used + char_len >= sizeof(line)) &&
            used > 0) {
            line[used] = '\0';
            ESP_RETURN_ON_ERROR(ssd1306_draw_text16(start_page + line_index * 2, 0, line),
                                TAG, "draw wrapped network error");
            line_index++;
            used = 0;
            pixel_width = 0;
            continue;
        }

        memcpy(line + used, p, char_len);
        used += char_len;
        pixel_width += char_width;
        p += char_len;
    }

    if (used > 0 && line_index < max_lines) {
        line[used] = '\0';
        ESP_RETURN_ON_ERROR(ssd1306_draw_text16(start_page + line_index * 2, 0, line),
                            TAG, "draw final wrapped network error");
    }
    return ESP_OK;
}

esp_err_t app_display_render_network_progress(const char *status, const char *detail)
{
    ESP_RETURN_ON_ERROR(ssd1306_clear(), TAG, "clear network progress");
    if (status != NULL && strcmp(status, "请求失败") == 0) {
        ESP_RETURN_ON_ERROR(ssd1306_draw_text16(0, 32, "请求失败"),
                            TAG, "draw network failure title");
        ESP_RETURN_ON_ERROR(draw_text16_wrapped(2, 3, detail),
                            TAG, "draw network failure detail");
        return ssd1306_flush();
    }

    ESP_RETURN_ON_ERROR(ssd1306_draw_text16(0, 24, "网络请求"), TAG, "draw network title");
    ESP_RETURN_ON_ERROR(ssd1306_draw_text16(2, 0, status != NULL ? status : "请稍候"),
                        TAG, "draw network status");
    ESP_RETURN_ON_ERROR(ssd1306_draw_text16(5, 0, detail != NULL ? detail : ""),
                        TAG, "draw network detail");
    return ssd1306_flush();
}

static esp_err_t draw_gamepad_mode_screen(void)
{
    ESP_RETURN_ON_ERROR(ssd1306_draw_text16_ccw(6, 0, "手柄模式"), TAG, "draw gamepad title");
    ESP_RETURN_ON_ERROR(ssd1306_draw_text16_ccw(48, 0, "十字ABXY"), TAG, "draw gamepad keys");
    ESP_RETURN_ON_ERROR(ssd1306_draw_text16_ccw(88, 0, "BX三秒"), TAG, "draw gamepad menu hint");
    return ESP_OK;
}

static esp_err_t draw_menu_list(const mode_view_t *view, uint32_t now_ms)
{
    const uint8_t base_page = 2;
    const uint8_t visible = 3;
    uint8_t start = 0;

    if (view->item_count > visible) {
        if (view->selected >= visible - 1) {
            start = view->selected - (visible - 1);
        }
        if (start + visible > view->item_count) {
            start = view->item_count - visible;
        }
    }

    for (uint8_t i = 0; i < visible && start + i < view->item_count; i++) {
        uint8_t idx = start + i;
        const char *label = view->items[idx];
        if (view->show_devices) {
            if (idx == view->selected) {
                int y = (base_page + i * 2) * 8 + 8;
                for (int dy = -3; dy <= 3; ++dy) {
                    int half_width = dy < 0 ? -dy : dy;
                    ESP_RETURN_ON_ERROR(ssd1306_fill_rect(2, y + dy, 5 - half_width, 1, true),
                                        TAG, "draw device cursor pixels");
                }
            }
        }
        if (view->show_devices) {
            ESP_RETURN_ON_ERROR(ssd1306_draw_text16_marquee(base_page + i * 2, 16, OLED_WIDTH - 16, label, s_device_scroll_step),
                                TAG, "draw device name pixels");
        } else {
            ESP_RETURN_ON_ERROR(ssd1306_draw_text16(base_page + i * 2, 0, label), TAG, "draw menu item");
        }
    }

    if (view->show_scrollbar) {
        ESP_RETURN_ON_ERROR(ssd1306_draw_scrollbar(base_page, visible * 2, view->item_count,
                                                   visible, view->selected),
                            TAG, "draw scrollbar");
    }

    bool highlight_visible = !view->show_devices || !view->blink_selected || (now_ms / 500) % 2 == 0;
    if (view->item_count > 0 && highlight_visible) {
        uint8_t sel_pos = (view->show_devices ? view->active_device : view->selected) - start;
        int invert_end_col = view->show_scrollbar ? OLED_WIDTH - 4 : OLED_WIDTH;
        ESP_RETURN_ON_ERROR(ssd1306_invert_area(base_page + sel_pos * 2,
                                                base_page + 2 + sel_pos * 2,
                                                0, invert_end_col),
                            TAG, "invert menu item");
    }

    return ESP_OK;
}

static esp_err_t draw_menu_view(const mode_view_t *view, const char *addr,
                                uint32_t now_ms, uint32_t *last_status_refresh_ms)
{
    if (view->show_devices) {
        if (!s_device_scroll_active || strcmp(view->title, s_device_scroll_title) != 0) {
            s_device_scroll_step = 0;
            snprintf(s_device_scroll_title, sizeof(s_device_scroll_title), "%s", view->title);
        }
        s_device_scroll_active = true;
        ESP_RETURN_ON_ERROR(ssd1306_draw_text16_marquee(0, 0, OLED_WIDTH, view->title, s_device_scroll_step),
                            TAG, "draw scrolling title pixels");
    } else {
        ESP_RETURN_ON_ERROR(ssd1306_draw_text16(0, 0, view->title), TAG, "draw menu title");
    }

    if (view->show_status) {
        char acc_line[24];
        char gyr_line[24];
        char bat_line[24];
        char imu_line[24];
        char mac_line[24];
        int battery_mv = 0;
        uint8_t battery_pct = app_display_read_battery_percent(&battery_mv);
        bmi270_sample_t imu = {0};
        esp_err_t imu_ret = bmi270_read_sample(&imu);

        snprintf(mac_line, sizeof(mac_line), "MAC %s", addr ? addr : "No Link");
        if (imu_ret == ESP_OK && imu.present) {
            snprintf(acc_line, sizeof(acc_line), "A%+6d%+6d%+6d",
                     imu.acc_x, imu.acc_y, imu.acc_z);
            snprintf(gyr_line, sizeof(gyr_line), "G%+6d%+6d%+6d",
                     imu.gyr_x, imu.gyr_y, imu.gyr_z);
            snprintf(imu_line, sizeof(imu_line), "220 ID%02X T %d.%dC",
                     imu.chip_id,
                     imu.temp_c10 / 10, imu.temp_c10 < 0 ? -(imu.temp_c10 % 10) : imu.temp_c10 % 10);
        } else {
            snprintf(acc_line, sizeof(acc_line), "A -- -- --");
            snprintf(gyr_line, sizeof(gyr_line), "G -- -- --");
            if (imu.chip_id == 0x26) {
                snprintf(imu_line, sizeof(imu_line), "BMI220 ID26");
            } else if (imu.chip_id != 0) {
                snprintf(imu_line, sizeof(imu_line), "BMI ID%02X", imu.chip_id);
            } else {
                snprintf(imu_line, sizeof(imu_line), "BMI220 No Link");
            }
        }
        if (battery_mv > 0) {
            snprintf(bat_line, sizeof(bat_line), "BAT %d.%02dV %u%%",
                     battery_mv / 1000, (battery_mv % 1000) / 10, battery_pct);
        } else {
            snprintf(bat_line, sizeof(bat_line), "BAT --");
        }

        ESP_RETURN_ON_ERROR(ssd1306_draw_text(2, 0, acc_line), TAG, "draw acc line");
        ESP_RETURN_ON_ERROR(ssd1306_draw_text(3, 0, gyr_line), TAG, "draw gyro line");
        ESP_RETURN_ON_ERROR(ssd1306_draw_text(4, 0, imu_line), TAG, "draw imu line");
        ESP_RETURN_ON_ERROR(ssd1306_draw_text(5, 0, bat_line), TAG, "draw battery line");
        ESP_RETURN_ON_ERROR(ssd1306_draw_text(6, 0, mac_line), TAG, "draw mac line");
        if (last_status_refresh_ms != NULL) {
            *last_status_refresh_ms = now_ms;
        }
    } else if (view->show_web_control) {
        ESP_RETURN_ON_ERROR(ssd1306_draw_text16(2, 0, "AP 自定义设置"), TAG, "draw web ap");
        ESP_RETURN_ON_ERROR(ssd1306_draw_text(3, 0, ""), TAG, "draw web spacer");
        ESP_RETURN_ON_ERROR(ssd1306_draw_text(4, 0, "URL 192.168.4.1"), TAG, "draw web url");
        ESP_RETURN_ON_ERROR(ssd1306_draw_text(6, 0, "F2 Exit"), TAG, "draw web exit");
    } else if (view->show_about) {
        static bool qr[QR_V2_SIZE][QR_V2_SIZE];
        ESP_RETURN_ON_ERROR(ssd1306_draw_text16(2, 0, "Powered"), TAG, "draw about powered");
        ESP_RETURN_ON_ERROR(ssd1306_draw_text16(4, 0, "By 乐乐"), TAG, "draw about author");
        ESP_RETURN_ON_ERROR(qrcode_v2_make(ABOUT_QR_URL, qr), TAG, "make about qr");
        ESP_RETURN_ON_ERROR(ssd1306_fill_rect(80, 16, 46, 46, true), TAG, "draw about qr background");
        for (int y = 0; y < QR_V2_SIZE; y++) {
            for (int x = 0; x < QR_V2_SIZE; x++) {
                if (qr[y][x]) {
                    ESP_RETURN_ON_ERROR(ssd1306_fill_rect(82 + x * 2, 18 + y * 2, 2, 2, false),
                                        TAG, "draw about qr module");
                }
            }
        }
    } else if (view->show_hid_confirm) {
        ESP_RETURN_ON_ERROR(ssd1306_draw_text16(2, 0, view->line1), TAG, "draw hid confirm line1");
        ESP_RETURN_ON_ERROR(ssd1306_draw_text16(5, 0, view->line2), TAG, "draw hid confirm line2");
    } else if (view->show_temperature_alert_config) {
        char threshold_text[12];
        int16_t threshold = view->temperature_alert_threshold_c10;
        int progress_width = (threshold - TEMPERATURE_ALERT_MIN_THRESHOLD_C10) * 64 /
                             (TEMPERATURE_ALERT_MAX_THRESHOLD_C10 -
                              TEMPERATURE_ALERT_MIN_THRESHOLD_C10);
        if (progress_width < 0) progress_width = 0;
        if (progress_width > 64) progress_width = 64;

        ESP_RETURN_ON_ERROR(ssd1306_draw_text16(2, 0, "提醒"),
                            TAG, "draw temperature alert label");
        ESP_RETURN_ON_ERROR(ssd1306_draw_text16(
                                2, 48,
                                view->temperature_alert_temporarily_disabled ? "临时关" :
                                view->temperature_alert_enabled ? "开启" : "关闭"),
                            TAG, "draw temperature alert state");
        if (view->temperature_alert_enabled) {
            ESP_RETURN_ON_ERROR(ssd1306_invert_area(2, 4, 44, 84),
                                TAG, "invert temperature alert enabled state");
        }

        ESP_RETURN_ON_ERROR(ssd1306_fill_rect(4, 36, 68, 10, true),
                            TAG, "draw temperature threshold bar border");
        ESP_RETURN_ON_ERROR(ssd1306_fill_rect(6, 38, 64, 6, false),
                            TAG, "clear temperature threshold bar");
        if (progress_width > 0) {
            ESP_RETURN_ON_ERROR(ssd1306_fill_rect(6, 38, progress_width, 6, true),
                                TAG, "fill temperature threshold bar");
        }
        snprintf(threshold_text, sizeof(threshold_text), "%d.%dC",
                 threshold / 10, threshold % 10);
        ESP_RETURN_ON_ERROR(ssd1306_draw_text16(4, 80, threshold_text),
                            TAG, "draw temperature threshold value");

        ESP_RETURN_ON_ERROR(ssd1306_draw_text(6, 0, "F1 On/Off  Up/Dn"),
                            TAG, "draw temperature alert operation hint");
        ESP_RETURN_ON_ERROR(ssd1306_draw_text(7, 0, "+/-1C  F2 Back"),
                            TAG, "draw temperature alert back hint");
    } else if (view->show_air_mouse_config) {
        ESP_RETURN_ON_ERROR(ssd1306_draw_text16(2, 0, view->line1), TAG, "draw air mouse config values");
        ESP_RETURN_ON_ERROR(ssd1306_draw_text(5, 0, view->line2), TAG, "draw air mouse config hint");
        ESP_RETURN_ON_ERROR(ssd1306_draw_text(6, 0, "Move test  F2 Exit"), TAG, "draw air mouse test hint");
    } else if (view->show_air_wheel_config) {
        ESP_RETURN_ON_ERROR(ssd1306_draw_text16(2, 0, view->line1), TAG, "draw air wheel config values");
        ESP_RETURN_ON_ERROR(ssd1306_draw_text(5, 0, view->line2), TAG, "draw air wheel config hint");
        ESP_RETURN_ON_ERROR(ssd1306_draw_text(6, 0, "F2 Exit"), TAG, "draw air wheel exit hint");
    } else if (view->show_data_collection) {
        ESP_RETURN_ON_ERROR(ssd1306_draw_text16(2, 0, view->line1), TAG, "draw data collection status");
        ESP_RETURN_ON_ERROR(ssd1306_draw_text(4, 0, view->line2), TAG, "draw data collection label");
        ESP_RETURN_ON_ERROR(ssd1306_draw_text(6, 0, "F1 Start/Stop F2 Exit"), TAG, "draw data collection hint");
    } else if (view->show_gesture_inference) {
        ESP_RETURN_ON_ERROR(ssd1306_draw_text16(2, 0, view->line1), TAG, "draw gesture inference status");
        ESP_RETURN_ON_ERROR(ssd1306_draw_text(4, 0, view->line2), TAG, "draw gesture inference result");
        ESP_RETURN_ON_ERROR(ssd1306_draw_text(6, 0, "F1 Pause/Resume F2 Exit"), TAG, "draw gesture inference hint");
    } else {
        ESP_RETURN_ON_ERROR(draw_menu_list(view, now_ms), TAG, "draw menu list");
    }

    return ESP_OK;
}

static esp_err_t draw_mode_view(const mode_view_t *view, bool connected)
{
    if (view->is_gamepad_mode) {
        return draw_gamepad_mode_screen();
    }

    uint8_t battery_pct = app_display_read_battery_percent(NULL);
    ESP_RETURN_ON_ERROR(ssd1306_draw_text16(0, 0, view->title), TAG, "draw mode title");
    ESP_RETURN_ON_ERROR(ssd1306_draw_battery_icon(0, 96, battery_pct), TAG, "draw battery icon");
    ESP_RETURN_ON_ERROR(ssd1306_draw_bluetooth_icon(0, 112, connected), TAG, "draw bluetooth icon");
    ESP_RETURN_ON_ERROR(ssd1306_draw_text16(2, 0, view->line1), TAG, "draw mode line1");
    ESP_RETURN_ON_ERROR(ssd1306_draw_text16(5, 0, view->line2), TAG, "draw mode line2");
    return ESP_OK;
}

uint16_t app_display_gif_frame_count(void)
{
    return FRAME_COUNT;
}

esp_err_t app_display_render_gif_frame(uint16_t frame_index)
{
    if (frame_index >= FRAME_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(ssd1306_draw_bitmap_full(frames[frame_index], FRAME_SIZE),
                        TAG, "draw gif frame");
    return ssd1306_flush();
}

esp_err_t app_display_render_view(const mode_view_t *view, bool connected, const char *addr,
                                  uint32_t now_ms, uint32_t *last_status_refresh_ms,
                                  uint32_t *last_header_refresh_ms)
{
    if (view == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!view->show_devices) s_device_scroll_active = false;
    ESP_RETURN_ON_ERROR(ssd1306_clear(), TAG, "clear screen");

    if (view->screen == APP_SCREEN_MENU) {
        ESP_RETURN_ON_ERROR(draw_menu_view(view, addr, now_ms, last_status_refresh_ms),
                            TAG, "draw menu view");
        if ((view->show_gesture_inference || view->show_devices) && last_status_refresh_ms != NULL) {
            *last_status_refresh_ms = now_ms;
        }
    } else {
        ESP_RETURN_ON_ERROR(draw_mode_view(view, connected), TAG, "draw mode view");
        if (last_header_refresh_ms != NULL) {
            *last_header_refresh_ms = now_ms;
        }
    }

    esp_err_t ret = ssd1306_flush();
    if (ret == ESP_OK && view->show_devices) ++s_device_scroll_step;
    return ret;
}
