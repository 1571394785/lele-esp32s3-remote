#include <stddef.h>
#include <string.h>

#include "ble_hid.h"
#include "esp_hid_common.h"

esp_hid_report_map_t *__real_esp_hid_parse_report_map(const uint8_t *data, size_t length);

// ESP-IDF creates GATT characteristics in parser order. Our Feature must live
// inside the mouse's Wheel Logical Collection, but adding it there would shift
// all existing Consumer/Gamepad attribute handles. Append only the new Feature
// in the parsed list so the wire descriptor and all old GATT handles stay valid.
esp_hid_report_map_t *__wrap_esp_hid_parse_report_map(const uint8_t *data, size_t length)
{
    esp_hid_report_map_t *map = __real_esp_hid_parse_report_map(data, length);
    if (map == NULL) return NULL;
    for (size_t i = 0; i < map->reports_len; ++i) {
        if (map->reports[i].report_id == BLE_HID_WHEEL_FEATURE_REPORT_ID &&
            map->reports[i].report_type == ESP_HID_REPORT_TYPE_FEATURE) {
            esp_hid_report_item_t feature = map->reports[i];
            memmove(&map->reports[i], &map->reports[i + 1],
                    (map->reports_len - i - 1) * sizeof(feature));
            map->reports[map->reports_len - 1] = feature;
            break;
        }
    }
    return map;
}
