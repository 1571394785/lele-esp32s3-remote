#pragma once

#include <stdint.h>

#define AIR_MOUSE_WHEEL_RESOLUTION 120

typedef struct {
    float velocity;
    float remainder;
    uint16_t resolution;
} air_mouse_scroll_t;

void air_mouse_scroll_reset(air_mouse_scroll_t *scroll);
// rate is tilt-compensated, deadzoned and sensitivity-scaled gyro input.
int8_t air_mouse_scroll_update(air_mouse_scroll_t *scroll, int32_t rate,
                              uint32_t elapsed_ms, uint16_t resolution);
