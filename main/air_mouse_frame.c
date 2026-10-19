#include "air_mouse_frame.h"

#include <math.h>
#include <string.h>

// bmi270.c configures +/-1000 dps and +/-16 g; match bmi220.h sensitivity.
#define GYRO_RADIANS_PER_LSB (0.01745329252f / 32.768f)
#define ACC_ONE_G 2048.0f
#define GRAVITY_CORRECTION_SECONDS 0.5f
#define MAX_SAMPLE_GAP_MS 100U
#define MIN_ROLL_PROJECTION 0.15f

static void normalize(float v[3])
{
    float norm = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (norm > 0.000001f) {
        for (int i = 0; i < 3; ++i) v[i] /= norm;
    }
}

void air_mouse_frame_reset(air_mouse_frame_t *frame)
{
    memset(frame, 0, sizeof(*frame));
    frame->roll_cos = 1.0f;
}

bool air_mouse_frame_update(air_mouse_frame_t *frame, const bmi270_sample_t *imu,
                            uint32_t timestamp_ms, int32_t *horizontal, int32_t *vertical)
{
    *horizontal = 0;
    *vertical = 0;
    if (!imu->present) return false;

    float accel[3] = {imu->acc_x, imu->acc_y, imu->acc_z};
    float accel_norm = sqrtf(accel[0] * accel[0] + accel[1] * accel[1] + accel[2] * accel[2]);
    // Strong translation/freefall must not rotate the cursor's reference frame.
    bool trust_accel = accel_norm >= 0.8f * ACC_ONE_G && accel_norm <= 1.2f * ACC_ONE_G;
    uint32_t elapsed_ms = timestamp_ms - frame->timestamp_ms;
    if (frame->initialized && elapsed_ms > MAX_SAMPLE_GAP_MS) {
        air_mouse_frame_reset(frame);
    }
    if (!frame->initialized) {
        if (!trust_accel) return false;
        for (int i = 0; i < 3; ++i) frame->gravity[i] = accel[i] / accel_norm;
        frame->initialized = true;
    } else if (elapsed_ms > 0) {
        float omega[3] = {imu->gyr_x * GYRO_RADIANS_PER_LSB,
                          imu->gyr_y * GYRO_RADIANS_PER_LSB,
                          imu->gyr_z * GYRO_RADIANS_PER_LSB};
        // A world-fixed gravity vector rotates by -omega in the device frame.
        // Small substeps keep the prediction stable during fast wrist turns.
        uint32_t steps = (elapsed_ms + 4U) / 5U;
        float dt = elapsed_ms * 0.001f / steps;
        for (uint32_t step = 0; step < steps; ++step) {
            float x = frame->gravity[0], y = frame->gravity[1], z = frame->gravity[2];
            frame->gravity[0] += (omega[2] * y - omega[1] * z) * dt;
            frame->gravity[1] += (omega[0] * z - omega[2] * x) * dt;
            frame->gravity[2] += (omega[1] * x - omega[0] * y) * dt;
            normalize(frame->gravity);
        }
        if (trust_accel) {
            float blend = elapsed_ms * 0.001f / (GRAVITY_CORRECTION_SECONDS + elapsed_ms * 0.001f);
            for (int i = 0; i < 3; ++i) {
                frame->gravity[i] += blend * (accel[i] / accel_norm - frame->gravity[i]);
            }
            normalize(frame->gravity);
        }
    }
    frame->timestamp_ms = timestamp_ms;

    // Existing mapping identifies X as the pointing/twist axis. Construct
    // screen-up in its YZ plane, then screen-right perpendicular to it.
    float projection = hypotf(frame->gravity[1], frame->gravity[2]);
    if (projection >= MIN_ROLL_PROJECTION) {
        frame->roll_sin = frame->gravity[1] / projection;
        frame->roll_cos = frame->gravity[2] / projection;
    }
    // Near vertical, gravity cannot determine roll: retain the last basis.
    // Pure twisting about the pointing axis must not move the cursor.
    *horizontal = (int32_t)lroundf(-(imu->gyr_y * frame->roll_sin + imu->gyr_z * frame->roll_cos));
    *vertical = (int32_t)lroundf(-(imu->gyr_y * frame->roll_cos - imu->gyr_z * frame->roll_sin));
    return true;
}
