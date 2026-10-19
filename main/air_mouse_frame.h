#pragma once

#include "bmi270.h"

typedef struct {
    float gravity[3];
    float roll_sin;
    float roll_cos;
    uint32_t timestamp_ms;
    bool initialized;
} air_mouse_frame_t;

void air_mouse_frame_reset(air_mouse_frame_t *frame);
// Returns false until a plausible gravity measurement is available.
bool air_mouse_frame_update(air_mouse_frame_t *frame, const bmi270_sample_t *imu,
                            uint32_t timestamp_ms, int32_t *horizontal, int32_t *vertical);
