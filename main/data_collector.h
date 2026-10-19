#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define DATA_COLLECTOR_SAMPLE_PERIOD_MS 10
#define DATA_COLLECTOR_LABEL "idle"

esp_err_t data_collector_init(void);
void data_collector_start(void);
void data_collector_stop(void);
void data_collector_toggle(void);
bool data_collector_is_running(void);
const char *data_collector_label(void);
uint32_t data_collector_sample_period_ms(void);
