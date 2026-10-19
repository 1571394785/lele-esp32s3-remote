#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bmi270.h"
#include "esp_err.h"

#define ATTITUDE_HISTORY_PERIOD_MS 10
#define ATTITUDE_HISTORY_WINDOW_MS 2000
#define ATTITUDE_HISTORY_CAPACITY (ATTITUDE_HISTORY_WINDOW_MS / ATTITUDE_HISTORY_PERIOD_MS)

typedef struct {
    uint32_t timestamp_ms;
    bmi270_sample_t imu;
} attitude_sample_t;

void attitude_history_init(void);
esp_err_t attitude_history_update(uint32_t now_ms);
bool attitude_history_latest(attitude_sample_t *out);
size_t attitude_history_copy_recent(attitude_sample_t *out, size_t max_count);
