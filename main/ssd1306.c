#include "ssd1306.h"

#include <stdbool.h>
#include <ctype.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"

#define OLED_I2C_PORT I2C_NUM_0
#define OLED_I2C_SPEED_HZ 400000
#define OLED_I2C_TIMEOUT_MS 1000

static const char *TAG = "ssd1306";

static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_oled_dev;

// 屏幕帧缓冲区：128 * (64/8) = 1024 字节
static uint8_t s_framebuf[OLED_WIDTH * (OLED_HEIGHT / 8)];

extern const uint8_t gb2312_font_start[] asm("_binary_gb2312_16x16_bin_start");
extern const uint8_t gb2312_font_end[] asm("_binary_gb2312_16x16_bin_end");
extern const uint8_t gb2312_map_start[] asm("_binary_gb2312_unicode_map_bin_start");
extern const uint8_t gb2312_map_end[] asm("_binary_gb2312_unicode_map_bin_end");

static const uint8_t UNKNOWN_16X16[32] = {
    0xFF, 0xFF, 0x01, 0x80, 0x39, 0x9C, 0x45, 0xA2,
    0x41, 0x82, 0x31, 0x8C, 0x09, 0x90, 0x11, 0x88,
    0x11, 0x88, 0x00, 0x80, 0x11, 0x88, 0x11, 0x88,
    0x00, 0x80, 0x01, 0x80, 0xFF, 0xFF, 0x00, 0x00,
};

static const uint8_t FONT_5X7[][5] = {
    [' ' - ' '] = {0x00, 0x00, 0x00, 0x00, 0x00},
    ['%' - ' '] = {0x23, 0x13, 0x08, 0x64, 0x62},
    ['+' - ' '] = {0x08, 0x08, 0x3E, 0x08, 0x08},
    ['-' - ' '] = {0x08, 0x08, 0x08, 0x08, 0x08},
    ['.' - ' '] = {0x00, 0x60, 0x60, 0x00, 0x00},
    ['/' - ' '] = {0x20, 0x10, 0x08, 0x04, 0x02},
    ['0' - ' '] = {0x3E, 0x51, 0x49, 0x45, 0x3E},
    ['1' - ' '] = {0x00, 0x42, 0x7F, 0x40, 0x00},
    ['2' - ' '] = {0x42, 0x61, 0x51, 0x49, 0x46},
    ['3' - ' '] = {0x21, 0x41, 0x45, 0x4B, 0x31},
    ['4' - ' '] = {0x18, 0x14, 0x12, 0x7F, 0x10},
    ['5' - ' '] = {0x27, 0x45, 0x45, 0x45, 0x39},
    ['6' - ' '] = {0x3C, 0x4A, 0x49, 0x49, 0x30},
    ['7' - ' '] = {0x01, 0x71, 0x09, 0x05, 0x03},
    ['8' - ' '] = {0x36, 0x49, 0x49, 0x49, 0x36},
    ['9' - ' '] = {0x06, 0x49, 0x49, 0x29, 0x1E},
    [':' - ' '] = {0x00, 0x36, 0x36, 0x00, 0x00},
    ['A' - ' '] = {0x7E, 0x11, 0x11, 0x11, 0x7E},
    ['B' - ' '] = {0x7F, 0x49, 0x49, 0x49, 0x36},
    ['C' - ' '] = {0x3E, 0x41, 0x41, 0x41, 0x22},
    ['D' - ' '] = {0x7F, 0x41, 0x41, 0x22, 0x1C},
    ['E' - ' '] = {0x7F, 0x49, 0x49, 0x49, 0x41},
    ['F' - ' '] = {0x7F, 0x09, 0x09, 0x09, 0x01},
    ['G' - ' '] = {0x3E, 0x41, 0x49, 0x49, 0x7A},
    ['H' - ' '] = {0x7F, 0x08, 0x08, 0x08, 0x7F},
    ['I' - ' '] = {0x00, 0x41, 0x7F, 0x41, 0x00},
    ['J' - ' '] = {0x20, 0x40, 0x41, 0x3F, 0x01},
    ['K' - ' '] = {0x7F, 0x08, 0x14, 0x22, 0x41},
    ['L' - ' '] = {0x7F, 0x40, 0x40, 0x40, 0x40},
    ['M' - ' '] = {0x7F, 0x02, 0x0C, 0x02, 0x7F},
    ['N' - ' '] = {0x7F, 0x04, 0x08, 0x10, 0x7F},
    ['O' - ' '] = {0x3E, 0x41, 0x41, 0x41, 0x3E},
    ['P' - ' '] = {0x7F, 0x09, 0x09, 0x09, 0x06},
    ['Q' - ' '] = {0x3E, 0x41, 0x51, 0x21, 0x5E},
    ['R' - ' '] = {0x7F, 0x09, 0x19, 0x29, 0x46},
    ['S' - ' '] = {0x46, 0x49, 0x49, 0x49, 0x31},
    ['T' - ' '] = {0x01, 0x01, 0x7F, 0x01, 0x01},
    ['U' - ' '] = {0x3F, 0x40, 0x40, 0x40, 0x3F},
    ['V' - ' '] = {0x1F, 0x20, 0x40, 0x20, 0x1F},
    ['W' - ' '] = {0x3F, 0x40, 0x38, 0x40, 0x3F},
    ['X' - ' '] = {0x63, 0x14, 0x08, 0x14, 0x63},
    ['Y' - ' '] = {0x07, 0x08, 0x70, 0x08, 0x07},
    ['Z' - ' '] = {0x61, 0x51, 0x49, 0x45, 0x43},
    ['_' - ' '] = {0x40, 0x40, 0x40, 0x40, 0x40},
};

static const char * const BLUETOOTH_ICON_16[] = {
    ".......#........",
    ".......##.......",
    ".......#.#......",
    "...#...#..#.....",
    "....#..#.#......",
    ".....#.#........",
    "......##........",
    ".......#........",
    "......##........",
    ".....#.#........",
    "....#..#.#......",
    "...#...#..#.....",
    ".......#.#......",
    ".......##.......",
    ".......#........",
    "................",
};

static esp_err_t ssd1306_write(const uint8_t *data, size_t len)
{
    return i2c_master_transmit(s_oled_dev, data, len, OLED_I2C_TIMEOUT_MS);
}

static esp_err_t ssd1306_cmd(uint8_t cmd)
{
    uint8_t data[] = {0x00, cmd};
    return ssd1306_write(data, sizeof(data));
}

static esp_err_t ssd1306_cmds(const uint8_t *cmds, size_t len)
{
    uint8_t buffer[32];

    if (len + 1 > sizeof(buffer)) {
        return ESP_ERR_INVALID_SIZE;
    }

    buffer[0] = 0x00;
    memcpy(&buffer[1], cmds, len);
    return ssd1306_write(buffer, len + 1);
}

static const uint8_t *font_for_char(char c)
{
    static const uint8_t unknown[5] = {0x7F, 0x49, 0x49, 0x49, 0x7F};

    if (c >= 'a' && c <= 'z') {
        c = (char)toupper((unsigned char)c);
    }

    if (c < ' ' || c > '_') {
        return unknown;
    }

    const uint8_t *glyph = FONT_5X7[c - ' '];
    if (glyph[0] == 0 && glyph[1] == 0 && glyph[2] == 0 && glyph[3] == 0 && glyph[4] == 0 && c != ' ') {
        return unknown;
    }

    return glyph;
}

static void draw_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) {
        return;
    }

    uint8_t *cell = &s_framebuf[(y / 8) * OLED_WIDTH + x];
    uint8_t mask = 1U << (y % 8);
    if (on) {
        *cell |= mask;
    } else {
        *cell &= (uint8_t)~mask;
    }
}

static void draw_pixel_ccw(int x, int y, bool on)
{
    if (x < 0 || x >= OLED_HEIGHT || y < 0 || y >= OLED_WIDTH) {
        return;
    }

    int physical_x = OLED_WIDTH - 1 - y;
    int physical_y = x;
    draw_pixel(physical_x, physical_y, on);
}

static void draw_line(int x0, int y0, int x1, int y1)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (true) {
        draw_pixel(x0, y0, true);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static bool decode_utf8(const char *text, uint32_t *codepoint, size_t *bytes)
{
    const uint8_t *s = (const uint8_t *)text;

    if (s[0] < 0x80) {
        *codepoint = s[0];
        *bytes = 1;
        return true;
    }

    if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        *codepoint = ((uint32_t)(s[0] & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
        *bytes = 2;
        return true;
    }

    if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *codepoint = ((uint32_t)(s[0] & 0x0F) << 12) |
                     ((uint32_t)(s[1] & 0x3F) << 6) |
                     (uint32_t)(s[2] & 0x3F);
        *bytes = 3;
        return true;
    }

    *codepoint = 0;
    *bytes = 1;
    return false;
}

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static bool unicode_to_gb2312(uint32_t codepoint, uint16_t *gb)
{
    const size_t record_size = 6;
    size_t count = (size_t)(gb2312_map_end - gb2312_map_start) / record_size;
    size_t left = 0;
    size_t right = count;

    while (left < right) {
        size_t mid = left + (right - left) / 2;
        const uint8_t *record = gb2312_map_start + mid * record_size;
        uint32_t record_codepoint = read_le32(record);

        if (record_codepoint == codepoint) {
            *gb = read_le16(record + 4);
            return true;
        }

        if (record_codepoint < codepoint) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }

    return false;
}

static const uint8_t *gb2312_glyph(uint16_t gb)
{
    uint8_t high = (uint8_t)(gb >> 8);
    uint8_t low = (uint8_t)(gb & 0xFF);

    if (high < 0xA1 || high > 0xFE || low < 0xA1 || low > 0xFE) {
        return UNKNOWN_16X16;
    }

    size_t index = ((size_t)(high - 0xA1) * 94 + (size_t)(low - 0xA1)) * 32;
    if (gb2312_font_start + index + 32 > gb2312_font_end) {
        return UNKNOWN_16X16;
    }

    return gb2312_font_start + index;
}

static esp_err_t ssd1306_draw_glyph16(int page, int col, const uint8_t glyph[32])
{
    if (page < 0 || page + 1 >= OLED_HEIGHT / 8 || col < 0 || col + 16 > OLED_WIDTH) {
        return ESP_ERR_INVALID_ARG;
    }

    for (int x = 0; x < 16; ++x) {
        s_framebuf[page * OLED_WIDTH + col + x] = glyph[x * 2];         // 上半行
        s_framebuf[(page + 1) * OLED_WIDTH + col + x] = glyph[x * 2 + 1]; // 下半行
    }

    return ESP_OK;
}

/**
 * I2C 总线恢复：当 SDA 被从设备拉低卡死时，
 * 通过 bit-bang SCL 时钟释放总线。
 */
static void i2c_bus_recover(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << OLED_SCL_GPIO) | (1ULL << OLED_SDA_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&io_conf);

    gpio_set_level(OLED_SCL_GPIO, 1);
    gpio_set_level(OLED_SDA_GPIO, 1);
    ets_delay_us(5);

    // 检测 SDA 是否被拉低
    gpio_set_direction(OLED_SDA_GPIO, GPIO_MODE_INPUT);
    if (gpio_get_level(OLED_SDA_GPIO) == 0) {
        ESP_LOGW(TAG, "SDA stuck low, sending clock pulses to recover");
        for (int i = 0; i < 16; i++) {
            gpio_set_level(OLED_SCL_GPIO, 0);
            ets_delay_us(5);
            gpio_set_level(OLED_SCL_GPIO, 1);
            ets_delay_us(5);
        }
        // 发送 STOP 条件
        gpio_set_direction(OLED_SDA_GPIO, GPIO_MODE_OUTPUT);
        gpio_set_level(OLED_SDA_GPIO, 0);
        ets_delay_us(5);
        gpio_set_level(OLED_SDA_GPIO, 1);
        ets_delay_us(5);
    }

    gpio_set_direction(OLED_SDA_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(OLED_SDA_GPIO, 1);
    gpio_set_level(OLED_SCL_GPIO, 1);
    ets_delay_us(10);
}

static esp_err_t ssd1306_do_init(void)
{
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = OLED_I2C_PORT,
        .scl_io_num = OLED_SCL_GPIO,
        .sda_io_num = OLED_SDA_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &s_i2c_bus), TAG, "i2c bus init failed");

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OLED_I2C_ADDR,
        .scl_speed_hz = OLED_I2C_SPEED_HZ,
    };

    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &dev_config, &s_oled_dev), TAG, "i2c add oled failed");

    const uint8_t init_cmds[] = {
        0xAE,       // Display off
        0x20, 0x00, // 水平寻址模式（用于整帧刷新）
        0xB0,
        0xC8,
        0x00,
        0x10,
        0x40,
        0x81, 0x7F,
        0xA1,
        0xA6,
        0xA8, 0x3F,
        0xA4,
        0xD3, 0x00,
        0xD5, 0x80,
        0xD9, 0xF1,
        0xDA, 0x12,
        0xDB, 0x40,
        0x8D, 0x14,
        0xAF,       // Display on
    };

    ESP_RETURN_ON_ERROR(ssd1306_cmds(init_cmds, sizeof(init_cmds)), TAG, "oled init commands failed");
    ESP_RETURN_ON_ERROR(ssd1306_clear(), TAG, "oled clear failed");
    ESP_RETURN_ON_ERROR(ssd1306_flush(), TAG, "oled flush failed");
    return ESP_OK;
}

esp_err_t ssd1306_init(void)
{
    // 最多重试 3 次，每次之间等待 200ms
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "OLED init retry %d/2", attempt);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        // 每次重试前先恢复 I2C 总线
        i2c_bus_recover();
        vTaskDelay(pdMS_TO_TICKS(50));

        esp_err_t ret = ssd1306_do_init();
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "OLED initialized: addr=0x%02X sda=%d scl=%d (attempt %d)",
                     OLED_I2C_ADDR, OLED_SDA_GPIO, OLED_SCL_GPIO, attempt + 1);
            return ESP_OK;
        }

        ESP_LOGW(TAG, "OLED init attempt %d failed: %s", attempt + 1, esp_err_to_name(ret));
        // 销毁 I2C 总线以便下次重试
        if (s_oled_dev) {
            i2c_master_bus_rm_device(s_oled_dev);
            s_oled_dev = NULL;
        }
        if (s_i2c_bus) {
            i2c_del_master_bus(s_i2c_bus);
            s_i2c_bus = NULL;
        }
    }

    ESP_LOGE(TAG, "OLED init failed after 3 attempts");
    return ESP_FAIL;
}

i2c_master_bus_handle_t ssd1306_get_i2c_bus(void)
{
    return s_i2c_bus;
}

esp_err_t ssd1306_clear(void)
{
    memset(s_framebuf, 0, sizeof(s_framebuf));
    return ESP_OK;
}

esp_err_t ssd1306_draw_text(int page, int col, const char *text)
{
    if (text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    while (*text != '\0' && col + 6 <= OLED_WIDTH) {
        const uint8_t *glyph = font_for_char(*text++);
        for (int i = 0; i < 5; ++i) {
            s_framebuf[page * OLED_WIDTH + col + i] = glyph[i];
        }
        s_framebuf[page * OLED_WIDTH + col + 5] = 0x00; // 间隔列
        col += 6;
    }

    return ESP_OK;
}

esp_err_t ssd1306_draw_text16(int page, int col, const char *text)
{
    if (text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    while (*text != '\0' && col < OLED_WIDTH) {
        uint32_t codepoint = 0;
        size_t consumed = 1;

        if (!decode_utf8(text, &codepoint, &consumed)) {
            codepoint = 0;
        }

        if (codepoint >= 0x80) {
            uint16_t gb = 0;
            const uint8_t *glyph = unicode_to_gb2312(codepoint, &gb) ? gb2312_glyph(gb) : UNKNOWN_16X16;
            if (col + 16 > OLED_WIDTH) {
                break;
            }
            ESP_RETURN_ON_ERROR(ssd1306_draw_glyph16(page, col, glyph), TAG, "draw gb2312 glyph failed");
            col += 16;
        } else {
            if (col + 8 > OLED_WIDTH) {
                break;
            }
            const uint8_t *ascii = font_for_char((char)codepoint);
            // 5x7 字形在 16px 行中垂直居中：向下偏移 4px
            // upper page: glyph << 4  |  lower page: glyph >> 4
            for (int i = 0; i < 5; ++i) {
                uint8_t g = ascii[i];
                s_framebuf[page * OLED_WIDTH + col + 2 + i] = g << 4;
                s_framebuf[(page + 1) * OLED_WIDTH + col + 2 + i] = g >> 4;
            }
            col += 8;
        }

        text += consumed;
    }

    return ESP_OK;
}

int ssd1306_text16_width(const char *text)
{
    int width = 0;
    if (!text) return 0;
    while (*text) {
        uint32_t codepoint = 0; size_t consumed = 1;
        decode_utf8(text, &codepoint, &consumed);
        width += codepoint >= 0x80 ? 16 : 8;
        text += consumed;
    }
    return width;
}

static void draw_text16_clipped(int page, int origin, int left, int right, const char *text)
{
    while (*text && origin < right) {
        uint32_t codepoint = 0; size_t consumed = 1;
        decode_utf8(text, &codepoint, &consumed);
        int width = codepoint >= 0x80 ? 16 : 8;
        const uint8_t *glyph = NULL;
        if (width == 16) {
            uint16_t gb = 0;
            glyph = unicode_to_gb2312(codepoint, &gb) ? gb2312_glyph(gb) : UNKNOWN_16X16;
        } else {
            glyph = font_for_char((char)codepoint);
        }
        for (int x = 0; x < width; ++x) {
            int col = origin + x;
            if (col < left || col >= right) continue;
            uint8_t upper = 0, lower = 0;
            if (width == 16) { upper = glyph[x * 2]; lower = glyph[x * 2 + 1]; }
            else if (x >= 2 && x < 7) { upper = glyph[x - 2] << 4; lower = glyph[x - 2] >> 4; }
            s_framebuf[page * OLED_WIDTH + col] = upper;
            s_framebuf[(page + 1) * OLED_WIDTH + col] = lower;
        }
        origin += width;
        text += consumed;
    }
}

esp_err_t ssd1306_draw_text16_marquee(int page, int col, int width, const char *text, uint32_t step)
{
    if (!text || page < 0 || page + 1 >= OLED_HEIGHT / 8 || col < 0 || width <= 0 || col + width > OLED_WIDTH)
        return ESP_ERR_INVALID_ARG;
    memset(&s_framebuf[page * OLED_WIDTH + col], 0, width);
    memset(&s_framebuf[(page + 1) * OLED_WIDTH + col], 0, width);
    int text_width = ssd1306_text16_width(text);
    if (text_width <= width) {
        draw_text16_clipped(page, col, col, col + width, text);
    } else {
        int cycle = text_width + 24;
        int offset = (int)(step % (uint32_t)cycle);
        draw_text16_clipped(page, col - offset, col, col + width, text);
        draw_text16_clipped(page, col - offset + cycle, col, col + width, text);
    }
    return ESP_OK;
}

esp_err_t ssd1306_draw_text16_ccw(int y, int x, const char *text)
{
    if (text == NULL || x < 0 || y < 0 || y + 16 > OLED_WIDTH) {
        return ESP_ERR_INVALID_ARG;
    }

    while (*text != '\0' && x < OLED_HEIGHT) {
        uint32_t codepoint = 0;
        size_t consumed = 1;

        if (!decode_utf8(text, &codepoint, &consumed)) {
            codepoint = 0;
        }

        if (codepoint >= 0x80) {
            if (x + 16 > OLED_HEIGHT) {
                break;
            }

            uint16_t gb = 0;
            const uint8_t *glyph = unicode_to_gb2312(codepoint, &gb) ? gb2312_glyph(gb) : UNKNOWN_16X16;
            for (int dx = 0; dx < 16; dx++) {
                for (int dy = 0; dy < 16; dy++) {
                    bool on = (glyph[dx * 2 + dy / 8] & (1U << (dy % 8))) != 0;
                    draw_pixel_ccw(x + dx, y + dy, on);
                }
            }
            x += 16;
        } else {
            if (x + 8 > OLED_HEIGHT) {
                break;
            }

            const uint8_t *ascii = font_for_char((char)codepoint);
            for (int dx = 0; dx < 5; dx++) {
                uint8_t g = ascii[dx];
                for (int dy = 0; dy < 7; dy++) {
                    if ((g & (1U << dy)) != 0) {
                        draw_pixel_ccw(x + 2 + dx, y + 4 + dy, true);
                    }
                }
            }
            x += 8;
        }

        text += consumed;
    }

    return ESP_OK;
}

static void draw_glyph16_scaled24(int y0, int col, const uint8_t glyph[32])
{
    for (int dy = 0; dy < 24; dy++) {
        int sy = dy * 16 / 24;
        for (int dx = 0; dx < 24; dx++) {
            int sx = dx * 16 / 24;
            bool on = (glyph[sx * 2 + sy / 8] & (1U << (sy % 8))) != 0;
            draw_pixel(col + dx, y0 + dy, on);
        }
    }
}

esp_err_t ssd1306_draw_text24(int y, int col, const char *text)
{
    if (text == NULL || y < 0 || y + 24 > OLED_HEIGHT || col < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    while (*text != '\0' && col < OLED_WIDTH) {
        uint32_t codepoint = 0;
        size_t consumed = 1;

        if (!decode_utf8(text, &codepoint, &consumed)) {
            codepoint = 0;
        }

        if (codepoint >= 0x80) {
            if (col + 24 > OLED_WIDTH) {
                break;
            }

            uint16_t gb = 0;
            const uint8_t *glyph = unicode_to_gb2312(codepoint, &gb) ? gb2312_glyph(gb) : UNKNOWN_16X16;
            draw_glyph16_scaled24(y, col, glyph);
            col += 24;
        } else {
            col += 12;
        }

        text += consumed;
    }

    return ESP_OK;
}

esp_err_t ssd1306_draw_pixel(int x, int y, bool on)
{
    draw_pixel(x, y, on);
    return ESP_OK;
}

esp_err_t ssd1306_draw_pixel_ccw(int x, int y, bool on)
{
    draw_pixel_ccw(x, y, on);
    return ESP_OK;
}

esp_err_t ssd1306_fill_rect(int x, int y, int w, int h, bool on)
{
    if (w <= 0 || h <= 0) {
        return ESP_OK;
    }

    for (int py = y; py < y + h; py++) {
        for (int px = x; px < x + w; px++) {
            draw_pixel(px, py, on);
        }
    }
    return ESP_OK;
}

esp_err_t ssd1306_fill_rect_ccw(int x, int y, int w, int h, bool on)
{
    if (w <= 0 || h <= 0) {
        return ESP_OK;
    }

    for (int py = y; py < y + h; py++) {
        for (int px = x; px < x + w; px++) {
            draw_pixel_ccw(px, py, on);
        }
    }
    return ESP_OK;
}

esp_err_t ssd1306_draw_bitmap_full(const uint8_t *bitmap, size_t len)
{
    if (bitmap == NULL || len != sizeof(s_framebuf)) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(s_framebuf, bitmap, sizeof(s_framebuf));
    return ESP_OK;
}

esp_err_t ssd1306_draw_bluetooth_icon(int page, int col, bool connected)
{
    if (page < 0 || page + 1 >= OLED_HEIGHT / 8 || col < 0 || col + 16 > OLED_WIDTH) {
        return ESP_ERR_INVALID_ARG;
    }

    int y0 = page * 8;
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            draw_pixel(col + x, y0 + y, false);
        }
    }

    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            if (BLUETOOTH_ICON_16[y][x] == '#') {
                draw_pixel(col + x, y0 + y, true);
            }
        }
    }

    if (!connected) {
        draw_line(col + 2, y0 + 14, col + 14, y0 + 2);
        draw_line(col + 3, y0 + 14, col + 14, y0 + 3);
    }

    return ESP_OK;
}

esp_err_t ssd1306_draw_battery_icon(int page, int col, uint8_t percent)
{
    if (page < 0 || page + 1 >= OLED_HEIGHT / 8 || col < 0 || col + 16 > OLED_WIDTH) {
        return ESP_ERR_INVALID_ARG;
    }
    if (percent > 100) {
        percent = 100;
    }

    int y0 = page * 8;
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            draw_pixel(col + x, y0 + y, false);
        }
    }

    // Body: 12x10 plus 2px positive terminal.
    for (int x = 0; x <= 11; x++) {
        draw_pixel(col + x, y0 + 3, true);
        draw_pixel(col + x, y0 + 12, true);
    }
    for (int y = 3; y <= 12; y++) {
        draw_pixel(col, y0 + y, true);
        draw_pixel(col + 11, y0 + y, true);
    }
    for (int y = 6; y <= 9; y++) {
        draw_pixel(col + 12, y0 + y, true);
        draw_pixel(col + 13, y0 + y, true);
    }

    uint8_t fill_cols = (uint8_t)((percent * 8 + 50) / 100);
    for (int x = 0; x < fill_cols; x++) {
        for (int y = 5; y <= 10; y++) {
            draw_pixel(col + 2 + x, y0 + y, true);
        }
    }

    return ESP_OK;
}

esp_err_t ssd1306_draw_scrollbar(uint8_t top_page, uint8_t page_count, uint8_t total, uint8_t visible, uint8_t selected)
{
    if (page_count == 0 || total <= visible || top_page + page_count > OLED_HEIGHT / 8) {
        return ESP_OK;
    }

    int x = OLED_WIDTH - 2;
    int y0 = top_page * 8;
    int h = page_count * 8;
    int thumb_h = (h * visible) / total;
    if (thumb_h < 6) {
        thumb_h = 6;
    }
    if (thumb_h > h) {
        thumb_h = h;
    }

    int range = h - thumb_h;
    int thumb_y = y0;
    if (total > 1 && range > 0) {
        thumb_y += (range * selected) / (total - 1);
    }

    for (int y = thumb_y; y < thumb_y + thumb_h; y++) {
        draw_pixel(x - 1, y, true);
        draw_pixel(x, y, true);
    }

    return ESP_OK;
}

esp_err_t ssd1306_invert_area(int start_page, int end_page, int col_start, int col_end)
{
    if (start_page < 0) start_page = 0;
    if (end_page > OLED_HEIGHT / 8) end_page = OLED_HEIGHT / 8;
    if (col_start < 0) col_start = 0;
    if (col_end > OLED_WIDTH) col_end = OLED_WIDTH;

    for (int page = start_page; page < end_page; page++) {
        for (int col = col_start; col < col_end; col++) {
            s_framebuf[page * OLED_WIDTH + col] ^= 0xFF;
        }
    }
    return ESP_OK;
}

esp_err_t ssd1306_flush(void)
{
    // 设置水平寻址模式：列 0~127，页 0~7
    ESP_RETURN_ON_ERROR(ssd1306_cmd(0x21), TAG, "flush col range failed");
    ESP_RETURN_ON_ERROR(ssd1306_cmd(0x00), TAG, "flush col start failed");
    ESP_RETURN_ON_ERROR(ssd1306_cmd(0x7F), TAG, "flush col end failed");
    ESP_RETURN_ON_ERROR(ssd1306_cmd(0x22), TAG, "flush page range failed");
    ESP_RETURN_ON_ERROR(ssd1306_cmd(0x00), TAG, "flush page start failed");
    ESP_RETURN_ON_ERROR(ssd1306_cmd(0x07), TAG, "flush page end failed");

    // 逐页发送帧缓冲区（每页 128 字节 + 1 字节数据前缀 = 129 字节）
    for (int page = 0; page < OLED_HEIGHT / 8; ++page) {
        uint8_t data[OLED_WIDTH + 1];
        data[0] = 0x40; // 数据模式
        memcpy(&data[1], &s_framebuf[page * OLED_WIDTH], OLED_WIDTH);
        ESP_RETURN_ON_ERROR(ssd1306_write(data, sizeof(data)), TAG, "flush page write failed");
    }

    return ESP_OK;
}
