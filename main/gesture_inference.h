#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define GESTURE_INFERENCE_SAMPLE_PERIOD_MS 10
#define GESTURE_INFERENCE_OUTPUT_PERIOD_MS 250
#define GESTURE_INFERENCE_PROB_TEMPERATURE 2.0f
#define GESTURE_INFERENCE_ACTION_THRESHOLD 0.70f

esp_err_t gesture_inference_init(void);
void gesture_inference_start(void);
void gesture_inference_stop(void);
void gesture_inference_toggle(void);
void gesture_inference_reset_window(void);
bool gesture_inference_is_running(void);
bool gesture_inference_take_event(int *label, float *probability, float *distance);
int gesture_inference_last_label(void);
float gesture_inference_last_probability(void);
float gesture_inference_last_distance(void);
int gesture_inference_sample_count(void);
uint32_t gesture_inference_sample_period_ms(void);
