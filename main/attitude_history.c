#include "attitude_history.h"

#include <string.h>

#define ATTITUDE_NO_SAMPLE_MS 0xFFFFFFFFU

static attitude_sample_t s_history[ATTITUDE_HISTORY_CAPACITY];
static uint16_t s_next_index;
static uint16_t s_count;
static uint32_t s_last_sample_ms = ATTITUDE_NO_SAMPLE_MS;

void attitude_history_init(void)
{
    memset(s_history, 0, sizeof(s_history));
    s_next_index = 0;
    s_count = 0;
    s_last_sample_ms = ATTITUDE_NO_SAMPLE_MS;
}

esp_err_t attitude_history_update(uint32_t now_ms)
{
    if (s_last_sample_ms != ATTITUDE_NO_SAMPLE_MS &&
        now_ms - s_last_sample_ms < ATTITUDE_HISTORY_PERIOD_MS) {
        return ESP_OK;
    }

    attitude_sample_t sample = {
        .timestamp_ms = now_ms,
    };
    esp_err_t ret = bmi270_read_sample(&sample.imu);
    if (ret != ESP_OK) {
        return ret;
    }

    s_history[s_next_index] = sample;
    s_next_index = (uint16_t)((s_next_index + 1) % ATTITUDE_HISTORY_CAPACITY);
    if (s_count < ATTITUDE_HISTORY_CAPACITY) {
        s_count++;
    }
    s_last_sample_ms = now_ms;
    return ESP_OK;
}

bool attitude_history_latest(attitude_sample_t *out)
{
    if (out == NULL || s_count == 0) {
        return false;
    }

    uint16_t idx = s_next_index == 0 ? ATTITUDE_HISTORY_CAPACITY - 1 : s_next_index - 1;
    *out = s_history[idx];
    return true;
}

size_t attitude_history_copy_recent(attitude_sample_t *out, size_t max_count)
{
    if (out == NULL || max_count == 0 || s_count == 0) {
        return 0;
    }

    size_t n = s_count < max_count ? s_count : max_count;
    uint16_t start = (uint16_t)((s_next_index + ATTITUDE_HISTORY_CAPACITY - n) %
                                ATTITUDE_HISTORY_CAPACITY);

    for (size_t i = 0; i < n; i++) {
        out[i] = s_history[(start + i) % ATTITUDE_HISTORY_CAPACITY];
    }

    return n;
}
