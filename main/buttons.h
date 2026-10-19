#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef enum {
    BUTTON_KEY_NONE = -1,
    BUTTON_KEY_UP = 0,
    BUTTON_KEY_DOWN,
    BUTTON_KEY_LEFT,
    BUTTON_KEY_RIGHT,
    BUTTON_KEY_FUNC1,
    BUTTON_KEY_FUNC2,
    BUTTON_KEY_FUNC3,
    BUTTON_KEY_FUNC4,
    BUTTON_KEY_COUNT,
} button_key_t;

#define BUTTON_MASK(key)        (1U << (key))
#define BUTTON_MASK_UP          BUTTON_MASK(BUTTON_KEY_UP)
#define BUTTON_MASK_DOWN        BUTTON_MASK(BUTTON_KEY_DOWN)
#define BUTTON_MASK_LEFT        BUTTON_MASK(BUTTON_KEY_LEFT)
#define BUTTON_MASK_RIGHT       BUTTON_MASK(BUTTON_KEY_RIGHT)
#define BUTTON_MASK_FUNC1       BUTTON_MASK(BUTTON_KEY_FUNC1)
#define BUTTON_MASK_FUNC2       BUTTON_MASK(BUTTON_KEY_FUNC2)
#define BUTTON_MASK_FUNC3       BUTTON_MASK(BUTTON_KEY_FUNC3)
#define BUTTON_MASK_FUNC4       BUTTON_MASK(BUTTON_KEY_FUNC4)

esp_err_t buttons_init(void);
button_key_t buttons_poll(void);
uint16_t buttons_poll_mask(void);
button_key_t buttons_first_key_from_mask(uint16_t mask);
const char *buttons_key_name_cn(button_key_t key);
