#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#define OLED_I2C_ADDR 0x3C
#define OLED_SCL_GPIO 17
#define OLED_SDA_GPIO 18
#define OLED_WIDTH 128
#define OLED_HEIGHT 64

esp_err_t ssd1306_init(void);
i2c_master_bus_handle_t ssd1306_get_i2c_bus(void);
esp_err_t ssd1306_clear(void);
esp_err_t ssd1306_draw_text(int page, int col, const char *text);
esp_err_t ssd1306_draw_text16(int page, int col, const char *text);
int ssd1306_text16_width(const char *text);
esp_err_t ssd1306_draw_text16_marquee(int page, int col, int width, const char *text, uint32_t step);
esp_err_t ssd1306_draw_text16_ccw(int y, int x, const char *text);
esp_err_t ssd1306_draw_text24(int y, int col, const char *text);
esp_err_t ssd1306_draw_pixel(int x, int y, bool on);
esp_err_t ssd1306_draw_pixel_ccw(int x, int y, bool on);
esp_err_t ssd1306_fill_rect(int x, int y, int w, int h, bool on);
esp_err_t ssd1306_fill_rect_ccw(int x, int y, int w, int h, bool on);
esp_err_t ssd1306_draw_bitmap_full(const uint8_t *bitmap, size_t len);
esp_err_t ssd1306_draw_bluetooth_icon(int page, int col, bool connected);
esp_err_t ssd1306_draw_battery_icon(int page, int col, uint8_t percent);
esp_err_t ssd1306_draw_scrollbar(uint8_t top_page, uint8_t page_count, uint8_t total, uint8_t visible, uint8_t selected);
esp_err_t ssd1306_invert_area(int start_page, int end_page, int col_start, int col_end);
esp_err_t ssd1306_flush(void);
