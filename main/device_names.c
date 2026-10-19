#include "device_names.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "nvs.h"
#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_names[DEVICE_NAME_COUNT][DEVICE_NAME_BYTES + 1];
static const char *const DEFAULT_NAMES[] = {"设备1", "设备2", "设备3"};
typedef struct { uint32_t version; char names[DEVICE_NAME_COUNT][DEVICE_NAME_BYTES + 1]; } names_record_t;

// Validate UTF-8 without accepting overlong encodings, surrogates or controls.
static unsigned char_size(const unsigned char *p, size_t remaining)
{
    if (p[0] >= 0x20 && p[0] < 0x7f) return 1;
    unsigned n = p[0] >= 0xc2 && p[0] <= 0xdf ? 2 :
                 p[0] >= 0xe0 && p[0] <= 0xef ? 3 :
                 p[0] >= 0xf0 && p[0] <= 0xf4 ? 4 : 0;
    if (!n || n > remaining) return 0;
    for (unsigned i = 1; i < n; ++i) if ((p[i] & 0xc0) != 0x80) return 0;
    if ((p[0] == 0xe0 && p[1] < 0xa0) || (p[0] == 0xed && p[1] >= 0xa0) ||
        (p[0] == 0xf0 && p[1] < 0x90) || (p[0] == 0xf4 && p[1] >= 0x90) ||
        (p[0] == 0xc2 && p[1] < 0xa0)) return 0;
    return n;
}

static bool normalize(const char *in, char *out, uint8_t slot)
{
    size_t length = strnlen(in, DEVICE_NAME_BYTES + 1);
    if (length > DEVICE_NAME_BYTES) return false;
    while (length && *in == ' ') { ++in; --length; }
    while (length && in[length - 1] == ' ') --length;
    unsigned count = 0;
    for (size_t i = 0; i < length;) {
        unsigned n = char_size((const unsigned char *)in + i, length - i);
        if (!n || ++count > DEVICE_NAME_CHARS) return false;
        i += n;
    }
    if (!length) { snprintf(out, DEVICE_NAME_BYTES + 1, "%s", DEFAULT_NAMES[slot]); return true; }
    memcpy(out, in, length); out[length] = 0;
    return true;
}

esp_err_t device_names_init(void)
{
    for (unsigned i = 0; i < DEVICE_NAME_COUNT; ++i) snprintf(s_names[i], sizeof(s_names[i]), "%s", DEFAULT_NAMES[i]);
    nvs_handle_t handle;
    esp_err_t ret = nvs_open("devnames", NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (ret != ESP_OK) return ret;
    names_record_t record = {0}, validated = {0}; size_t size = sizeof(record);
    ret = nvs_get_blob(handle, "names", &record, &size); nvs_close(handle);
    if (ret != ESP_OK || size != sizeof(record) || record.version != 1) return ESP_OK;
    for (uint8_t i = 0; i < DEVICE_NAME_COUNT; ++i)
        if (!normalize(record.names[i], validated.names[i], i)) return ESP_OK;
    memcpy(s_names, validated.names, sizeof(s_names));
    return ESP_OK;
}

void device_names_get(uint8_t slot, char *out, size_t size)
{
    if (!out || !size) return;
    portENTER_CRITICAL(&s_lock);
    snprintf(out, size, "%s", slot < DEVICE_NAME_COUNT ? s_names[slot] : "");
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t device_names_save(const char names[DEVICE_NAME_COUNT][DEVICE_NAME_BYTES + 1])
{
    if (!names) return ESP_ERR_INVALID_ARG;
    names_record_t record = {.version = 1};
    for (uint8_t i = 0; i < DEVICE_NAME_COUNT; ++i)
        if (!normalize(names[i], record.names[i], i)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t ret = nvs_open("devnames", NVS_READWRITE, &handle);
    if (ret != ESP_OK) return ret;
    ret = nvs_set_blob(handle, "names", &record, sizeof(record));
    if (ret == ESP_OK) ret = nvs_commit(handle);
    nvs_close(handle);
    if (ret == ESP_OK) {
        portENTER_CRITICAL(&s_lock); memcpy(s_names, record.names, sizeof(s_names)); portEXIT_CRITICAL(&s_lock);
    }
    return ret;
}

esp_err_t device_names_reset(void)
{
    char names[DEVICE_NAME_COUNT][DEVICE_NAME_BYTES + 1] = {{0}};
    return device_names_save(names);
}

