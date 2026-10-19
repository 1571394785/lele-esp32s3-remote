#include "buttons.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define BUTTON_ACTIVE_LEVEL 0
#define BUTTON_DEBOUNCE_MS 10
#define BUTTON_SCAN_PERIOD_US 5000
#define BUTTON_EVENT_QUEUE_LENGTH 32

typedef struct {
    button_key_t key;
    gpio_num_t gpio;
    const char *name_cn;
    uint32_t raw_changed_ms;
    bool raw_pressed;
    bool debounced_pressed;
} button_config_t;

static const char *TAG = "buttons";
static QueueHandle_t s_mask_queue;
static esp_timer_handle_t s_scan_timer;
static uint16_t s_queued_mask;
static uint16_t s_polled_mask;

static button_config_t s_buttons[] = {
    {BUTTON_KEY_UP,    GPIO_NUM_5,  "上",     0, false, false},
    {BUTTON_KEY_DOWN,  GPIO_NUM_6,  "下",     0, false, false},
    {BUTTON_KEY_LEFT,  GPIO_NUM_7,  "左",     0, false, false},
    {BUTTON_KEY_RIGHT, GPIO_NUM_4,  "右",     0, false, false},
    {BUTTON_KEY_FUNC1, GPIO_NUM_41, "功能一", 0, false, false},
    {BUTTON_KEY_FUNC2, GPIO_NUM_40, "功能二", 0, false, false},
    {BUTTON_KEY_FUNC3, GPIO_NUM_42, "功能三", 0, false, false},
    {BUTTON_KEY_FUNC4, GPIO_NUM_39, "功能四", 0, false, false},
};

static void scan_buttons(void *arg)
{
    (void)arg;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    uint16_t mask = 0;
    for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); ++i) {
        button_config_t *button = &s_buttons[i];
        bool pressed = gpio_get_level(button->gpio) == BUTTON_ACTIVE_LEVEL;
        if (pressed != button->raw_pressed) {
            button->raw_pressed = pressed;
            button->raw_changed_ms = now_ms;
        }
        if (now_ms - button->raw_changed_ms >= BUTTON_DEBOUNCE_MS)
            button->debounced_pressed = button->raw_pressed;
        if (button->debounced_pressed) mask |= BUTTON_MASK(button->key);
    }
    // Preserve both edges of short taps while OLED I2C or a BLE transition is
    // busy. If full, retry the latest state next scan, including releases.
    if (mask != s_queued_mask && xQueueSend(s_mask_queue, &mask, 0) == pdTRUE)
        s_queued_mask = mask;
}

esp_err_t buttons_init(void)
{
    if (s_scan_timer) return ESP_OK;
    uint64_t pin_mask = 0;
    for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); ++i)
        pin_mask |= 1ULL << s_buttons[i].gpio;
    gpio_config_t io_config = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_config), TAG, "gpio config failed");
    s_mask_queue = xQueueCreate(BUTTON_EVENT_QUEUE_LENGTH, sizeof(uint16_t));
    if (!s_mask_queue) return ESP_ERR_NO_MEM;
    const esp_timer_create_args_t timer = {
        .callback = scan_buttons,
        .name = "button_scan",
        .skip_unhandled_events = true,
    };
    esp_err_t ret = esp_timer_create(&timer, &s_scan_timer);
    if (ret == ESP_OK) ret = esp_timer_start_periodic(s_scan_timer, BUTTON_SCAN_PERIOD_US);
    if (ret != ESP_OK) {
        if (s_scan_timer) esp_timer_delete(s_scan_timer);
        vQueueDelete(s_mask_queue);
        s_scan_timer = NULL; s_mask_queue = NULL;
        return ret;
    }
    ESP_LOGI(TAG, "Buttons initialized: 5 ms sampling, 10 ms debounce, queued edges");
    return ESP_OK;
}

uint16_t buttons_poll_mask(void)
{
    if (s_mask_queue) {
        uint16_t mask;
        if (xQueueReceive(s_mask_queue, &mask, 0) == pdTRUE) s_polled_mask = mask;
    }
    return s_polled_mask;
}

button_key_t buttons_first_key_from_mask(uint16_t mask)
{
    for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); ++i) {
        if ((mask & BUTTON_MASK(s_buttons[i].key)) != 0) {
            return s_buttons[i].key;
        }
    }

    return BUTTON_KEY_NONE;
}

button_key_t buttons_poll(void)
{
    return buttons_first_key_from_mask(buttons_poll_mask());
}

const char *buttons_key_name_cn(button_key_t key)
{
    for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); ++i) {
        if (s_buttons[i].key == key) {
            return s_buttons[i].name_cn;
        }
    }

    return "无";
}
