#include "data_collector.h"

#include <inttypes.h>
#include <stdio.h>

#include "bmi270.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gesture_inference.h"

#define DATA_COLLECTOR_TASK_STACK 4096
#define DATA_COLLECTOR_TASK_PRIORITY 4

static const char *TAG = "data_collector";
static TaskHandle_t s_task;
static volatile bool s_running;

static void data_collector_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();

    while (true) {
        if (!s_running) {
            last_wake = xTaskGetTickCount();
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        bmi270_sample_t sample = {0};
        uint32_t timestamp_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (bmi270_read_raw_sample(&sample) == ESP_OK && sample.present) {
            printf("%" PRIu32 ",%d,%d,%d,%d,%d,%d,%s\r\n",
                   timestamp_ms,
                   sample.acc_x, sample.acc_y, sample.acc_z,
                   sample.gyr_x, sample.gyr_y, sample.gyr_z,
                   DATA_COLLECTOR_LABEL);
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(DATA_COLLECTOR_SAMPLE_PERIOD_MS));
    }
}

esp_err_t data_collector_init(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    BaseType_t ret = xTaskCreate(data_collector_task, "data_collector",
                                 DATA_COLLECTOR_TASK_STACK, NULL,
                                 DATA_COLLECTOR_TASK_PRIORITY, &s_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "failed to create task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void data_collector_start(void)
{
    if (!s_running) {
        gesture_inference_stop();
        esp_log_level_set("*", ESP_LOG_NONE);
        printf("timestamp_ms,acc_x,acc_y,acc_z,gyr_x,gyr_y,gyr_z,label\r\n");
        s_running = true;
    }
}

void data_collector_stop(void)
{
    if (s_running) {
        s_running = false;
        esp_log_level_set("*", ESP_LOG_INFO);
    }
}

void data_collector_toggle(void)
{
    if (s_running) {
        data_collector_stop();
    } else {
        data_collector_start();
    }
}

bool data_collector_is_running(void)
{
    return s_running;
}

const char *data_collector_label(void)
{
    return DATA_COLLECTOR_LABEL;
}

uint32_t data_collector_sample_period_ms(void)
{
    return DATA_COLLECTOR_SAMPLE_PERIOD_MS;
}
