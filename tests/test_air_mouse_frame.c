#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "air_mouse_frame.h"

static void check_near(int32_t actual, int32_t expected)
{
    assert(labs((long)actual - expected) <= 3);
}

int main(void)
{
    const float radians = 0.01745329252f;
    // Same screen motion must survive roll, inversion, and combined pitch/roll.
    for (int pitch = -70; pitch <= 70; pitch += 35) {
        for (int roll = -180; roll <= 180; roll += 15) {
            float s = sinf(roll * radians), c = cosf(roll * radians);
            float p = pitch * radians;
            for (int axis = 0; axis < 2; ++axis) {
                air_mouse_frame_t frame;
                air_mouse_frame_reset(&frame);
                bmi270_sample_t imu = {
                    .present = true,
                    .acc_x = (int16_t)lroundf(-2048 * sinf(p)),
                    .acc_y = (int16_t)lroundf(2048 * cosf(p) * s),
                    .acc_z = (int16_t)lroundf(2048 * cosf(p) * c),
                    .gyr_y = (int16_t)lroundf(3000 * (axis == 0 ? s : c)),
                    .gyr_z = (int16_t)lroundf(3000 * (axis == 0 ? c : -s)),
                };
                int32_t x, y;
                assert(air_mouse_frame_update(&frame, &imu, 10, &x, &y));
                check_near(x, axis == 0 ? -3000 : 0);
                check_near(y, axis == 1 ? -3000 : 0);
                imu.gyr_y = imu.gyr_z = 0;
                imu.gyr_x = 6000; // Twist about the pointing axis.
                assert(air_mouse_frame_update(&frame, &imu, 10, &x, &y));
                assert(x == 0 && y == 0);
            }
        }
    }

    air_mouse_frame_t frame;
    air_mouse_frame_reset(&frame);
    bmi270_sample_t imu = {.present = true, .acc_z = 2048};
    int32_t x, y;
    assert(air_mouse_frame_update(&frame, &imu, 10, &x, &y));
    // Rapid 90-degree roll with exact synthetic gravity must track without
    // waiting for the accelerometer low-pass filter to catch up.
    imu.gyr_x = (int16_t)lroundf(180 * 32.768f);
    for (int i = 1; i <= 50; ++i) {
        float angle = i * 1.8f * radians;
        imu.acc_y = (int16_t)lroundf(2048 * sinf(angle));
        imu.acc_z = (int16_t)lroundf(2048 * cosf(angle));
        assert(air_mouse_frame_update(&frame, &imu, 10 + i * 10, &x, &y));
        assert(x == 0 && y == 0);
        assert(fabsf(frame.roll_sin - sinf(angle)) < 0.01f);
        assert(fabsf(frame.roll_cos - cosf(angle)) < 0.01f);
    }
    // Translation at 2g should not corrupt the established sideways grip.
    imu.gyr_x = 0;
    imu.acc_y = 0;
    imu.acc_z = 4096;
    assert(air_mouse_frame_update(&frame, &imu, 520, &x, &y));
    assert(frame.roll_sin > 0.99f);

    // A gap must reacquire gravity, including timestamp wraparound.
    imu.acc_z = 2048;
    imu.gyr_z = 3000;
    assert(air_mouse_frame_update(&frame, &imu, UINT32_MAX - 4, &x, &y));
    check_near(x, -3000);
    assert(air_mouse_frame_update(&frame, &imu, 5, &x, &y));
    check_near(x, -3000);

    // Pointing straight up/down is singular: retain a finite, stable basis.
    imu.acc_x = 2048;
    imu.acc_z = 0;
    assert(air_mouse_frame_update(&frame, &imu, 1000, &x, &y));
    check_near(x, -3000);
    check_near(y, 0);

    air_mouse_frame_reset(&frame);
    imu.acc_x = imu.acc_y = imu.acc_z = 0;
    assert(!air_mouse_frame_update(&frame, &imu, 1010, &x, &y));
    assert(x == 0 && y == 0);
    imu.present = false;
    assert(!air_mouse_frame_update(&frame, &imu, 1020, &x, &y));

    // A rotated pair of int16 readings can exceed int16 range: no wrap/sign flip.
    air_mouse_frame_reset(&frame);
    imu = (bmi270_sample_t){.present = true, .acc_y = 1448, .acc_z = 1448,
                            .gyr_y = INT16_MAX, .gyr_z = INT16_MAX};
    assert(air_mouse_frame_update(&frame, &imu, 10, &x, &y));
    check_near(x, -46340);
    check_near(y, 0);
    puts("PASS: 250 grip/motion cases, dynamic roll, translation, gaps, wrap, vertical, missing gravity and full-scale rotation");
    return 0;
}
