#include "gesture_inference.h"

#include <math.h>
#include <string.h>

#include "bmi270.h"
#include "data_collector.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gesture_model.h"

#define GESTURE_INFERENCE_TASK_STACK 6144
#define GESTURE_INFERENCE_TASK_PRIORITY 4

static const char *TAG = "gesture_inference";
static TaskHandle_t s_task;
static volatile bool s_running;
static bool s_window_initialized;
static int s_last_label;
static float s_last_probability;
static float s_last_distance;
static volatile bool s_event_pending;
static int s_event_label;
static float s_event_probability;
static float s_event_distance;

static float s_window[GESTURE_WINDOW_SIZE][6];
static int s_sample_count;
static int s_write_index;

static float score_to_probability(float best_dist)
{
    float feat[GESTURE_NUM_FEATURES] = {0};
    float distances[GESTURE_NUM_CLASSES] = {0};
    float max_score = -1e30f;

    gesture_extract_features(s_window, GESTURE_WINDOW_SIZE, feat);
    gesture_normalize(feat);

    for (int c = 0; c < GESTURE_NUM_CLASSES; c++) {
        float d = 0;
        for (int f = 0; f < GESTURE_NUM_FEATURES; f++) {
            float t = feat[f] - gesture_centroids[c][f];
            d += t * t;
        }
        distances[c] = sqrtf(d);
        float score = -distances[c] / GESTURE_INFERENCE_PROB_TEMPERATURE;
        if (score > max_score) {
            max_score = score;
        }
    }

    float denom = 0;
    float best_exp = 0;
    for (int c = 0; c < GESTURE_NUM_CLASSES; c++) {
        float e = expf((-distances[c] / GESTURE_INFERENCE_PROB_TEMPERATURE) - max_score);
        denom += e;
        if (fabsf(distances[c] - best_dist) < 0.0001f) {
            best_exp = e;
        }
    }

    return denom > 0 ? best_exp / denom : 0;
}

void gesture_inference_reset_window(void)
{
    memset(s_window, 0, sizeof(s_window));
    s_sample_count = 0;
    s_write_index = 0;
    s_last_label = 0;
    s_last_probability = 0;
    s_last_distance = 0;
    s_event_pending = false;
    s_event_label = 0;
    s_event_probability = 0;
    s_event_distance = 0;
    s_window_initialized = true;
}

static void gesture_inference_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t last_output_ms = 0;
    uint32_t last_log_ms = 0;
    uint32_t read_fail_count = 0;

    while (true) {
        if (!s_running) {
            last_wake = xTaskGetTickCount();
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        bmi270_sample_t sample = {0};
        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (bmi270_read_raw_sample(&sample) == ESP_OK && sample.present) {
            s_window[s_write_index][0] = sample.acc_x;
            s_window[s_write_index][1] = sample.acc_y;
            s_window[s_write_index][2] = sample.acc_z;
            s_window[s_write_index][3] = sample.gyr_x;
            s_window[s_write_index][4] = sample.gyr_y;
            s_window[s_write_index][5] = sample.gyr_z;
            s_write_index = (s_write_index + 1) % GESTURE_WINDOW_SIZE;
            if (s_sample_count < GESTURE_WINDOW_SIZE) {
                s_sample_count++;
            }

            if (s_sample_count >= GESTURE_WINDOW_SIZE &&
                now_ms - last_output_ms >= GESTURE_INFERENCE_OUTPUT_PERIOD_MS) {
                float dist = 0;
                int label = gesture_predict(s_window, GESTURE_WINDOW_SIZE, &dist);
                float prob = score_to_probability(dist);
                s_last_label = label;
                s_last_distance = dist;
                s_last_probability = prob;
                if (prob >= GESTURE_INFERENCE_ACTION_THRESHOLD) {
                    s_event_label = label;
                    s_event_probability = prob;
                    s_event_distance = dist;
                    s_event_pending = true;
                }
                last_output_ms = now_ms;
            }
        } else {
            read_fail_count++;
        }

        if (now_ms - last_log_ms >= 1000) {
            ESP_LOGI(TAG, "running=%d samples=%d label=%d prob=%.2f dist=%.2f read_fail=%lu",
                     s_running ? 1 : 0,
                     s_sample_count,
                     s_last_label,
                     s_last_probability,
                     s_last_distance,
                     (unsigned long)read_fail_count);
            read_fail_count = 0;
            last_log_ms = now_ms;
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(GESTURE_INFERENCE_SAMPLE_PERIOD_MS));
    }
}

esp_err_t gesture_inference_init(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    gesture_inference_reset_window();

    BaseType_t ret = xTaskCreate(gesture_inference_task, "gesture_infer",
                                 GESTURE_INFERENCE_TASK_STACK, NULL,
                                 GESTURE_INFERENCE_TASK_PRIORITY, &s_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "failed to create task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void gesture_inference_start(void)
{
    if (!s_running) {
        data_collector_stop();
        if (!s_window_initialized) {
            gesture_inference_reset_window();
        }
        s_running = true;
        ESP_LOGI(TAG, "started");
    }
}

void gesture_inference_stop(void)
{
    if (s_running) {
        s_running = false;
        ESP_LOGI(TAG, "stopped");
    }
}

void gesture_inference_toggle(void)
{
    if (s_running) {
        gesture_inference_stop();
    } else {
        gesture_inference_start();
    }
}

bool gesture_inference_is_running(void)
{
    return s_running;
}

bool gesture_inference_take_event(int *label, float *probability, float *distance)
{
    if (!s_event_pending) {
        return false;
    }

    if (label != NULL) {
        *label = s_event_label;
    }
    if (probability != NULL) {
        *probability = s_event_probability;
    }
    if (distance != NULL) {
        *distance = s_event_distance;
    }
    s_event_pending = false;
    return true;
}

int gesture_inference_last_label(void)
{
    return s_last_label;
}

float gesture_inference_last_probability(void)
{
    return s_last_probability;
}

float gesture_inference_last_distance(void)
{
    return s_last_distance;
}

int gesture_inference_sample_count(void)
{
    return s_sample_count;
}

uint32_t gesture_inference_sample_period_ms(void)
{
    return GESTURE_INFERENCE_SAMPLE_PERIOD_MS;
}
