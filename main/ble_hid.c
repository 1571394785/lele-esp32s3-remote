#include "ble_hid.h"
#include "device_names.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <inttypes.h>
#include <stdio.h>

#include "battery_adc.h"
#include "air_mouse_scroll.h"
#include "hid_peer_state.h"
#include "ble_peer_identity.h"
#include "esp_bt.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_hidd.h"
#include "esp_hidd_gatts.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BLE_HID_REPORT_MAP_INDEX 0
#define BLE_HID_KEYBOARD_REPORT_ID 1
#define BLE_HID_MOUSE_REPORT_ID 2
#define BLE_HID_CONSUMER_REPORT_ID 3
#define BLE_HID_GAMEPAD_REPORT_ID 4
// Wheel alone shares a Logical Collection with Resolution Multiplier.
// Keep the four-byte input report intact; feature follows it because ESP-IDF's
// report-map parser finalizes a report whenever its ID changes.
#define BLE_HID_PRECISE_WHEEL_DESCRIPTOR \
    0x09, 0x38, 0xA1, 0x02, \
    0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, \
    0x75, 0x08, 0x95, 0x01, 0x81, 0x06, \
    0x85, BLE_HID_WHEEL_FEATURE_REPORT_ID, \
    0x09, 0x48, 0x15, 0x00, 0x25, 0x01, \
    0x35, 0x01, 0x45, AIR_MOUSE_WHEEL_RESOLUTION, \
    0x75, 0x08, 0x95, 0x01, 0xB1, 0x02, \
    0x35, 0x00, 0x45, 0x00, 0xC0
#define BLE_HID_DRAG_STEPS 5
#define BLE_HID_BATTERY_FIRST_NOTIFY_MS 1000
#define BLE_HID_BATTERY_NOTIFY_INTERVAL_MS 60000
#define BLE_HID_BATTERY_TASK_STACK 4096
#define BLE_HID_DEVICE_NVS_NAMESPACE "bledev"
#define BLE_HID_DEVICE_NVS_CURRENT "current"
#define BLE_HID_DEVICE_NVS_FULL_MODE "fullmode"
#define BLE_HID_DEVICE_NVS_OWN_ADDR "randaddr"

static const char *TAG = "ble_hid";
static const char *DEVICE_NAME = "乐乐牌遥控器";

static esp_hidd_dev_t *s_hid_dev;
static volatile bool s_connected;
static volatile bool s_link_up;
static volatile bool s_hid_started;
static volatile bool s_transition;
static volatile bool s_adv_starting;
static volatile bool s_adv_stopping;
static volatile bool s_whitelist_busy;
static volatile bool s_privacy_busy;
static volatile bool s_filter_pending;
static volatile esp_bt_status_t s_privacy_status;
static volatile esp_bt_status_t s_whitelist_status;
static volatile bool s_advertising;
static bool s_adv_config_done;
static bool s_scan_rsp_config_done;
static bool s_allow_auto_advertise;
static bool s_pairing_discoverable;
static bool s_pairing_new_keys;
static uint8_t s_selected_device_slot;
static ble_hid_device_slot_t s_device_slots[BLE_HID_DEVICE_SLOT_COUNT];
static esp_bd_addr_t s_connected_bda;
static char s_connected_addr[18]; // "XX:XX:XX:XX:XX:XX"
static TaskHandle_t s_battery_task;
static TickType_t s_next_battery_notify_tick;
static uint8_t s_last_battery_level;
static bool s_full_hid_mode;
static TickType_t s_mouse_retry_tick;
static bool s_mouse_report_pending;
static bool s_have_random_addr;
static esp_bd_addr_t s_random_addr;

static uint8_t s_hid_service_uuid[] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x12, 0x18, 0x00, 0x00,
};

static const uint8_t HID_REPORT_MAP_COMPAT[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x06,       // Usage (Keyboard)
    0xA1, 0x01,       // Collection (Application)
    0x85, BLE_HID_KEYBOARD_REPORT_ID,
    0x05, 0x07,       // Usage Page (Keyboard/Keypad)
    0x19, 0xE0,       // Usage Minimum (Keyboard LeftControl)
    0x29, 0xE7,       // Usage Maximum (Keyboard Right GUI)
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x08,
    0x81, 0x02,       // Input (Modifier byte)
    0x95, 0x01,
    0x75, 0x08,
    0x81, 0x01,       // Input (Reserved byte)
    0x95, 0x06,
    0x75, 0x08,
    0x15, 0x00,
    0x25, 0x65,
    0x05, 0x07,
    0x19, 0x00,
    0x29, 0x65,
    0x81, 0x00,       // Input (6 key array)
    0xC0,             // End Collection

    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x02,       // Usage (Mouse)
    0xA1, 0x01,       // Collection (Application)
    0x85, BLE_HID_MOUSE_REPORT_ID,
    0x09, 0x01,       // Usage (Pointer)
    0xA1, 0x00,       // Collection (Physical)
    0x05, 0x09,       // Usage Page (Button)
    0x19, 0x01,
    0x29, 0x03,
    0x15, 0x00,
    0x25, 0x01,
    0x95, 0x03,
    0x75, 0x01,
    0x81, 0x02,       // Input (3 buttons)
    0x95, 0x01,
    0x75, 0x05,
    0x81, 0x01,       // Input (5 bits padding)
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x30,       // Usage (X)
    0x15, 0x81,       // Logical Minimum (-127)
    0x25, 0x7F,       // Logical Maximum (127)
    0x75, 0x08,       // Report Size (8)
    0x95, 0x01,       // Report Count (1)
    0x81, 0x06,       // Input (Data, Var, Rel)
    0x09, 0x31,       // Usage (Y)
    0x15, 0x81,       // Logical Minimum (-127)
    0x25, 0x7F,       // Logical Maximum (127)
    0x75, 0x08,       // Report Size (8)
    0x95, 0x01,       // Report Count (1)
    0x81, 0x06,       // Input (Data, Var, Rel)
    BLE_HID_PRECISE_WHEEL_DESCRIPTOR,
    0xC0,             // End Physical Collection
    0xC0,             // End Application Collection

    0x05, 0x0C,       // Usage Page (Consumer)
    0x09, 0x01,       // Usage (Consumer Control)
    0xA1, 0x01,       // Collection (Application)
    0x85, BLE_HID_CONSUMER_REPORT_ID,
    0x15, 0x00,       // Logical Minimum (0)
    0x26, 0xFF, 0x03, // Logical Maximum (0x03FF)
    0x19, 0x00,       // Usage Minimum (0)
    0x2A, 0xFF, 0x03, // Usage Maximum (0x03FF)
    0x75, 0x10,       // Report Size (16)
    0x95, 0x01,       // Report Count (1)
    0x81, 0x00,       // Input (Data, Array, Abs)
    0xC0              // End Application Collection
};

static const uint8_t HID_REPORT_MAP_FULL[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x06,       // Usage (Keyboard)
    0xA1, 0x01,       // Collection (Application)
    0x85, BLE_HID_KEYBOARD_REPORT_ID,
    0x05, 0x07,       // Usage Page (Keyboard/Keypad)
    0x19, 0xE0,       // Usage Minimum (Keyboard LeftControl)
    0x29, 0xE7,       // Usage Maximum (Keyboard Right GUI)
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x08,
    0x81, 0x02,       // Input (Modifier byte)
    0x95, 0x01,
    0x75, 0x08,
    0x81, 0x01,       // Input (Reserved byte)
    0x95, 0x06,
    0x75, 0x08,
    0x15, 0x00,
    0x25, 0x65,
    0x05, 0x07,
    0x19, 0x00,
    0x29, 0x65,
    0x81, 0x00,       // Input (6 key array)
    0xC0,             // End Collection

    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x02,       // Usage (Mouse)
    0xA1, 0x01,       // Collection (Application)
    0x85, BLE_HID_MOUSE_REPORT_ID,
    0x09, 0x01,       // Usage (Pointer)
    0xA1, 0x00,       // Collection (Physical)
    0x05, 0x09,       // Usage Page (Button)
    0x19, 0x01,
    0x29, 0x03,
    0x15, 0x00,
    0x25, 0x01,
    0x95, 0x03,
    0x75, 0x01,
    0x81, 0x02,       // Input (3 buttons)
    0x95, 0x01,
    0x75, 0x05,
    0x81, 0x01,       // Input (5 bits padding)
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x30,       // Usage (X)
    0x15, 0x81,       // Logical Minimum (-127)
    0x25, 0x7F,       // Logical Maximum (127)
    0x75, 0x08,       // Report Size (8)
    0x95, 0x01,       // Report Count (1)
    0x81, 0x06,       // Input (Data, Var, Rel)
    0x09, 0x31,       // Usage (Y)
    0x15, 0x81,       // Logical Minimum (-127)
    0x25, 0x7F,       // Logical Maximum (127)
    0x75, 0x08,       // Report Size (8)
    0x95, 0x01,       // Report Count (1)
    0x81, 0x06,       // Input (Data, Var, Rel)
    BLE_HID_PRECISE_WHEEL_DESCRIPTOR,
    0xC0,             // End Physical Collection
    0xC0,             // End Application Collection

    0x05, 0x0C,       // Usage Page (Consumer)
    0x09, 0x01,       // Usage (Consumer Control)
    0xA1, 0x01,       // Collection (Application)
    0x85, BLE_HID_CONSUMER_REPORT_ID,
    0x15, 0x00,       // Logical Minimum (0)
    0x26, 0xFF, 0x03, // Logical Maximum (0x03FF)
    0x19, 0x00,       // Usage Minimum (0)
    0x2A, 0xFF, 0x03, // Usage Maximum (0x03FF)
    0x75, 0x10,       // Report Size (16)
    0x95, 0x01,       // Report Count (1)
    0x81, 0x00,       // Input (Data, Array, Abs)
    0xC0,             // End Application Collection

    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x05,       // Usage (Game Pad)
    0xA1, 0x01,       // Collection (Application)
    0x85, BLE_HID_GAMEPAD_REPORT_ID,
    0x05, 0x09,       // Usage Page (Button)
    0x19, 0x01,       // Usage Minimum (Button 1 / A)
    0x29, 0x0A,       // Usage Maximum (Button 10)
    0x15, 0x00,       // Logical Minimum (0)
    0x25, 0x01,       // Logical Maximum (1)
    0x75, 0x01,       // Report Size (1)
    0x95, 0x0A,       // Report Count (10)
    0x81, 0x02,       // Input (Data, Var, Abs)
    0x75, 0x01,       // Report Size (1)
    0x95, 0x06,       // Report Count (6)
    0x81, 0x03,       // Input (Const, Var, Abs)
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x39,       // Usage (Hat switch)
    0x15, 0x00,       // Logical Minimum (0)
    0x25, 0x07,       // Logical Maximum (7)
    0x35, 0x00,       // Physical Minimum (0)
    0x46, 0x3B, 0x01, // Physical Maximum (315)
    0x65, 0x14,       // Unit (English Rotation, Degrees)
    0x75, 0x04,       // Report Size (4)
    0x95, 0x01,       // Report Count (1)
    0x81, 0x42,       // Input (Data, Var, Abs, Null State)
    0x65, 0x00,       // Unit (None)
    0x75, 0x04,       // Report Size (4)
    0x95, 0x01,       // Report Count (1)
    0x81, 0x03,       // Input (Const, Var, Abs)
    0x09, 0x30,       // Usage (X)
    0x09, 0x31,       // Usage (Y)
    0x09, 0x33,       // Usage (Rx)
    0x09, 0x34,       // Usage (Ry)
    0x15, 0x00,       // Logical Minimum (0)
    0x26, 0xFF, 0x00, // Logical Maximum (255)
    0x75, 0x08,       // Report Size (8)
    0x95, 0x04,       // Report Count (4)
    0x81, 0x02,       // Input (Data, Var, Abs)
    0xC0              // End Application Collection
};

static esp_hid_raw_report_map_t s_hid_report_maps[] = {
    {
        .data = HID_REPORT_MAP_COMPAT,
        .len = sizeof(HID_REPORT_MAP_COMPAT),
    },
};

static esp_hid_device_config_t s_hid_config = {
    .vendor_id = 0x16C0,
    .product_id = 0x05DF,
    .version = 0x0101,
    .device_name = "乐乐牌遥控器",
    .manufacturer_name = "ESP32",
    .serial_number = "0001",
    .report_maps = s_hid_report_maps,
    .report_maps_len = 1,
};

static esp_ble_adv_data_t s_adv_data = {
    .set_scan_rsp = false,
    .include_name = false,
    .include_txpower = true,
    .min_interval = 0x0006,
    .max_interval = 0x0010,
    .appearance = ESP_BLE_APPEARANCE_GENERIC_HID,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = sizeof(s_hid_service_uuid),
    .p_service_uuid = s_hid_service_uuid,
    .flag = ESP_BLE_ADV_FLAG_BREDR_NOT_SPT,
};

static esp_ble_adv_data_t s_scan_rsp_data = {
    .set_scan_rsp = true,
    .include_name = true,
    .include_txpower = false,
};

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x30,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static char slot_key(uint8_t slot)
{
    return (char)('0' + slot);
}

static void load_device_slots(void)
{
    nvs_handle_t h;
    memset(s_device_slots, 0, sizeof(s_device_slots));
    s_selected_device_slot = 0;

    if (nvs_open(BLE_HID_DEVICE_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }

    nvs_get_u8(h, BLE_HID_DEVICE_NVS_CURRENT, &s_selected_device_slot);
    if (s_selected_device_slot >= BLE_HID_DEVICE_SLOT_COUNT) {
        s_selected_device_slot = 0;
    }

    for (uint8_t i = 0; i < BLE_HID_DEVICE_SLOT_COUNT; i++) {
        char key[6] = {'s', 'l', 'o', 't', slot_key(i), '\0'};
        size_t len = sizeof(s_device_slots[i]);
        esp_err_t ret = nvs_get_blob(h, key, &s_device_slots[i], &len);
        if (ret != ESP_OK || len != sizeof(s_device_slots[i])) {
            memset(&s_device_slots[i], 0, sizeof(s_device_slots[i]));
        }
    }

    nvs_close(h);
}

static void load_hid_mode(void)
{
    nvs_handle_t h;
    uint8_t full_mode = 0;

    if (nvs_open(BLE_HID_DEVICE_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, BLE_HID_DEVICE_NVS_FULL_MODE, &full_mode);
        nvs_close(h);
    }

    s_full_hid_mode = full_mode != 0;
    s_hid_report_maps[0].data = s_full_hid_mode ? HID_REPORT_MAP_FULL : HID_REPORT_MAP_COMPAT;
    s_hid_report_maps[0].len = s_full_hid_mode ? sizeof(HID_REPORT_MAP_FULL) : sizeof(HID_REPORT_MAP_COMPAT);
    ESP_LOGI(TAG, "HID mode: %s", s_full_hid_mode ? "full" : "compatible");
}

static void load_own_address(void)
{
    nvs_handle_t h;
    size_t len = sizeof(s_random_addr);
    s_have_random_addr = false;

    if (nvs_open(BLE_HID_DEVICE_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, BLE_HID_DEVICE_NVS_OWN_ADDR, s_random_addr, &len) == ESP_OK &&
            len == sizeof(s_random_addr)) {
            s_have_random_addr = true;
            s_adv_params.own_addr_type = BLE_ADDR_TYPE_RANDOM;
        }
        nvs_close(h);
    }
}

static esp_err_t save_hid_mode(bool full_mode)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(BLE_HID_DEVICE_NVS_NAMESPACE, NVS_READWRITE, &h),
                        TAG, "open hid mode nvs failed");
    esp_err_t ret = nvs_set_u8(h, BLE_HID_DEVICE_NVS_FULL_MODE, full_mode ? 1 : 0);
    if (ret == ESP_OK) {
        ret = nvs_commit(h);
    }
    nvs_close(h);
    return ret;
}

static esp_err_t save_selected_slot(void)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(BLE_HID_DEVICE_NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "open slot selection failed");
    esp_err_t ret = nvs_set_u8(h, BLE_HID_DEVICE_NVS_CURRENT, s_selected_device_slot);
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}

static esp_err_t save_device_slot(uint8_t slot)
{
    if (slot >= BLE_HID_DEVICE_SLOT_COUNT) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(BLE_HID_DEVICE_NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "open device slot failed");
    char key[6] = {'s', 'l', 'o', 't', slot_key(slot), '\0'};
    esp_err_t ret = nvs_set_blob(h, key, &s_device_slots[slot], sizeof(s_device_slots[slot]));
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}

static void remove_bond_for_addr(const uint8_t addr[6])
{
    hid_peer_forget(addr);
    int dev_num = esp_ble_get_bond_device_num();
    if (dev_num <= 0) {
        return;
    }

    esp_ble_bond_dev_t bonds[dev_num];
    if (esp_ble_get_bond_device_list(&dev_num, bonds) != ESP_OK) {
        return;
    }

    for (int i = 0; i < dev_num; i++) {
        if (memcmp(bonds[i].bd_addr, addr, 6) == 0) {
            esp_ble_remove_bond_device(bonds[i].bd_addr);
            return;
        }
    }
}

// All UI transitions wait for completion events, never a guessed delay.
static esp_err_t wait_flag_clear(volatile bool *flag)
{
    TickType_t start = xTaskGetTickCount();
    while (*flag) {
        if (xTaskGetTickCount() - start > pdMS_TO_TICKS(3000)) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

static esp_err_t stop_advertising_now(void)
{
    ESP_RETURN_ON_ERROR(wait_flag_clear(&s_adv_starting), TAG, "advertising start timeout");
    if (s_advertising && !s_adv_stopping) {
        s_adv_stopping = true;
        esp_err_t ret = esp_ble_gap_stop_advertising();
        if (ret != ESP_OK) { s_adv_stopping = false; return ret; }
    }
    ESP_RETURN_ON_ERROR(wait_flag_clear(&s_adv_stopping), TAG, "advertising stop timeout");
    return s_advertising ? ESP_FAIL : ESP_OK;
}

static bool bond_has_irk(const esp_ble_bond_dev_t *bond)
{
    if (!(bond->bond_key.key_mask & ESP_BLE_ID_KEY_MASK)) return false;
    uint8_t nonzero = 0;
    for (unsigned i = 0; i < 16; ++i) nonzero |= bond->bond_key.pid_key.irk[i];
    return nonzero != 0;
}

static bool address_matches_bond(const esp_ble_bond_dev_t *bond, const uint8_t addr[6])
{
    if (memcmp(bond->bd_addr, addr, 6) == 0) return true;
    if (!bond_has_irk(bond)) return false;
    return memcmp(bond->bond_key.pid_key.static_addr, addr, 6) == 0 ||
           ble_peer_rpa_matches(bond->bond_key.pid_key.irk, addr);
}

static bool get_slot_bond(uint8_t slot, esp_ble_bond_dev_t *out)
{
    if (!s_device_slots[slot].valid) return false;
    int count = esp_ble_get_bond_device_num();
    if (count <= 0) return false;
    esp_ble_bond_dev_t bonds[count];
    memset(bonds, 0, sizeof(bonds)); // ESP-IDF ORs key flags into caller storage.
    if (esp_ble_get_bond_device_list(&count, bonds) != ESP_OK) return false;
    for (int i = 0; i < count; ++i) {
        if (address_matches_bond(&bonds[i], s_device_slots[slot].addr)) {
            *out = bonds[i];
            return true;
        }
    }
    return false;
}

static bool address_matches_slot(uint8_t slot, const uint8_t addr[6])
{
    if (!s_device_slots[slot].valid) return false;
    if (memcmp(s_device_slots[slot].addr, addr, 6) == 0) return true;
    esp_ble_bond_dev_t bond;
    return get_slot_bond(slot, &bond) && address_matches_bond(&bond, addr);
}

static bool selected_filter_identity(esp_bd_addr_t address, uint8_t *type)
{
    esp_ble_bond_dev_t bond;
    bool private_peer = get_slot_bond(s_selected_device_slot, &bond) && bond_has_irk(&bond);
    if (private_peer) {
        memcpy(address, bond.bond_key.pid_key.static_addr, 6);
        *type = (uint8_t)bond.bond_key.pid_key.addr_type & 1;
    } else {
        memcpy(address, s_device_slots[s_selected_device_slot].addr, 6);
        *type = s_device_slots[s_selected_device_slot].addr_type & 1;
    }
    return private_peer;
}

static esp_err_t configure_slot_filter(bool pairing)
{
    ESP_RETURN_ON_ERROR(wait_flag_clear(&s_privacy_busy), TAG, "previous privacy update timeout");
    ESP_RETURN_ON_ERROR(wait_flag_clear(&s_whitelist_busy), TAG, "previous filter update timeout");
    // Resolve rotating peers only. Fixed-address hosts use the same direct
    // controller whitelist as before multi-device support, without privacy
    // procedures changing the controller's connection identity handling.
    esp_bd_addr_t identity = {0};
    uint8_t type = 0;
    bool private_peer = !pairing && selected_filter_identity(identity, &type);
    bool resolve_peer = private_peer &&
                        ((s_device_slots[s_selected_device_slot].addr[0] & 0xC0) == 0x40);
    s_privacy_busy = true;
    esp_err_t ret = esp_ble_gap_config_local_privacy(resolve_peer);
    if (ret != ESP_OK) { s_privacy_busy = false; return ret; }
    ESP_RETURN_ON_ERROR(wait_flag_clear(&s_privacy_busy), TAG, "enable controller resolution timeout");
    if (s_privacy_status != ESP_BT_STATUS_SUCCESS) return ESP_FAIL;

    s_whitelist_busy = true;
    ret = esp_ble_gap_clear_whitelist();
    if (ret != ESP_OK) { s_whitelist_busy = false; return ret; }
    ESP_RETURN_ON_ERROR(wait_flag_clear(&s_whitelist_busy), TAG, "clear filter timeout");
    if (s_whitelist_status != ESP_BT_STATUS_SUCCESS) return ESP_FAIL;
    if (!pairing) {
        if (resolve_peer) {
            // Device privacy accepts either a stable identity (Windows) or a
            // valid rotating RPA (iPhone), but still requires the whitelist.
            s_privacy_busy = true;
            ret = esp_ble_gap_set_privacy_mode(type, identity, ESP_BLE_DEVICE_PRIVACY_MODE);
            if (ret != ESP_OK) { s_privacy_busy = false; return ret; }
            ESP_RETURN_ON_ERROR(wait_flag_clear(&s_privacy_busy), TAG, "peer privacy mode timeout");
            if (s_privacy_status != ESP_BT_STATUS_SUCCESS) return ESP_FAIL;
        }
        s_whitelist_busy = true;
        ret = esp_ble_gap_update_whitelist(true, identity,
                  type ? BLE_WL_ADDR_TYPE_RANDOM : BLE_WL_ADDR_TYPE_PUBLIC);
        if (ret != ESP_OK) { s_whitelist_busy = false; return ret; }
        ESP_RETURN_ON_ERROR(wait_flag_clear(&s_whitelist_busy), TAG, "set filter timeout");
        if (s_whitelist_status != ESP_BT_STATUS_SUCCESS) return ESP_FAIL;
    }
    // Reconnects ALWAYS use controller filtering. Never admit an old host and
    // repeatedly disconnect it in software: that triggers a Windows retry storm.
    s_adv_params.adv_filter_policy = pairing ? ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY :
                                               ADV_FILTER_ALLOW_SCAN_ANY_CON_WLST;
    ESP_LOGI(TAG, "device %u filter: %s", s_selected_device_slot + 1,
             pairing ? "pairing discovery" : resolve_peer ?
             "controller identity whitelist with RPA resolution" : "fixed-address controller whitelist");
    return ESP_OK;
}

static esp_err_t configure_discoverable(bool discoverable)
{
    s_adv_data.flag = ESP_BLE_ADV_FLAG_BREDR_NOT_SPT;
    if (discoverable) {
        s_adv_data.flag |= ESP_BLE_ADV_FLAG_GEN_DISC;
    }
    s_adv_config_done = false;
    return esp_ble_gap_config_adv_data(&s_adv_data);
}

static bool addr_is_any_slot(const uint8_t addr[6])
{
    for (uint8_t i = 0; i < BLE_HID_DEVICE_SLOT_COUNT; i++) {
        if (address_matches_slot(i, addr)) {
            return true;
        }
    }
    return false;
}

static bool addr_is_allowed_for_security(const uint8_t addr[6])
{
    if (s_transition) return false;
    if (s_filter_pending && s_link_up)
        return address_matches_slot(s_selected_device_slot, addr);
    if (!s_allow_auto_advertise) return false;
    if (s_pairing_discoverable) return true;
    return address_matches_slot(s_selected_device_slot, addr);
}

static void start_advertising_if_ready(void)
{
    if (s_transition || s_link_up || !s_allow_auto_advertise ||
        !s_adv_config_done || !s_scan_rsp_config_done || s_advertising ||
        s_adv_starting || s_adv_stopping || s_whitelist_busy || s_privacy_busy) return;
    if (!s_pairing_discoverable && !s_device_slots[s_selected_device_slot].valid) return;
    s_adv_starting = true;
    if (esp_ble_gap_start_advertising(&s_adv_params) != ESP_OK) s_adv_starting = false;
}

static bool tick_reached(TickType_t now, TickType_t target)
{
    return (int32_t)(now - target) >= 0;
}

static void battery_notify_task(void *arg)
{
    (void)arg;

    while (true) {
        if (s_connected && s_hid_dev != NULL) {
            TickType_t now = xTaskGetTickCount();
            if (s_next_battery_notify_tick == 0 || tick_reached(now, s_next_battery_notify_tick)) {
                int battery_mv = 0;
                if (battery_adc_read_actual_mv(&battery_mv) == ESP_OK) {
                    s_last_battery_level = battery_adc_percent_from_mv(battery_mv);
                }
                esp_hidd_dev_battery_set(s_hid_dev, s_last_battery_level);
                ESP_LOGI(TAG, "battery level notified: %u%%", s_last_battery_level);
                s_next_battery_notify_tick = now + pdMS_TO_TICKS(BLE_HID_BATTERY_NOTIFY_INTERVAL_MS);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_SET_LOCAL_PRIVACY_COMPLETE_EVT:
        s_privacy_status = param->local_privacy_cmpl.status;
        if (s_privacy_status != ESP_BT_STATUS_SUCCESS) s_allow_auto_advertise = false;
        s_privacy_busy = false;
        start_advertising_if_ready();
        break;
    case ESP_GAP_BLE_SET_PRIVACY_MODE_COMPLETE_EVT:
        s_privacy_status = param->set_privacy_mode_cmpl.status;
        if (s_privacy_status != ESP_BT_STATUS_SUCCESS) s_allow_auto_advertise = false;
        s_privacy_busy = false;
        start_advertising_if_ready();
        break;
    case ESP_GAP_BLE_UPDATE_WHITELIST_COMPLETE_EVT:
        s_whitelist_status = param->update_whitelist_cmpl.status;
        s_whitelist_busy = false;
        if (s_whitelist_status != ESP_BT_STATUS_SUCCESS) {
            s_allow_auto_advertise = false;
            ESP_LOGE(TAG, "device whitelist update failed");
        }
        start_advertising_if_ready();
        break;
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        if (param->adv_data_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            s_adv_config_done = true;
            start_advertising_if_ready();
        } else {
            ESP_LOGE(TAG, "adv data config failed: %d", param->adv_data_cmpl.status);
        }
        break;

    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        if (param->scan_rsp_data_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            s_scan_rsp_config_done = true;
            start_advertising_if_ready();
        } else {
            ESP_LOGE(TAG, "scan rsp config failed: %d", param->scan_rsp_data_cmpl.status);
        }
        break;

    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            s_advertising = true;
            ESP_LOGI(TAG, "advertising started");
        } else {
            ESP_LOGE(TAG, "advertising start failed: %d", param->adv_start_cmpl.status);
        }
        s_adv_starting = false;
        break;

    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        if (param->adv_stop_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            s_advertising = false;
            ESP_LOGI(TAG, "advertising stopped");
        } else {
            ESP_LOGE(TAG, "advertising stop failed: %d", param->adv_stop_cmpl.status);
        }
        s_adv_stopping = false;
        break;

    case ESP_GAP_BLE_SEC_REQ_EVT:
        if (addr_is_allowed_for_security(param->ble_security.ble_req.bd_addr)) {
            ESP_LOGI(TAG, "security request accepted");
            esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        } else {
            s_allow_auto_advertise = false;
            ESP_LOGW(TAG, "security request rejected; advertising paused");
            esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, false);
            esp_ble_gap_disconnect(param->ble_security.ble_req.bd_addr);
        }
        break;

    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
        ESP_LOGI(TAG, "passkey notify: %06" PRIu32, param->ble_security.key_notif.passkey);
        break;

    case ESP_GAP_BLE_KEY_EVT:
        if (s_pairing_discoverable && s_link_up && !s_transition)
            s_pairing_new_keys = true;
        break;

    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        if (param->ble_security.auth_cmpl.success) {
            if (!addr_is_allowed_for_security(param->ble_security.auth_cmpl.bd_addr) ||
                (s_pairing_discoverable && !s_pairing_new_keys &&
                 addr_is_any_slot(param->ble_security.auth_cmpl.bd_addr))) {
                // Fail closed after one rejected link. Re-advertising here
                // makes Windows retry indefinitely and repeatedly rebuild HID.
                s_allow_auto_advertise = false;
                ESP_LOGW(TAG, "unexpected bonded reconnect; advertising paused until explicit retry");
                esp_ble_gap_disconnect(param->ble_security.auth_cmpl.bd_addr);
                break;
            }
            s_connected = true;
            ESP_LOGI(TAG, "authenticated (%s)", s_pairing_discoverable ? "new pairing" : "bonded reconnect");
            snprintf(s_connected_addr, sizeof(s_connected_addr),
                     "%02x:%02x:%02x:%02x:%02x:%02x",
                     param->ble_security.auth_cmpl.bd_addr[0],
                     param->ble_security.auth_cmpl.bd_addr[1],
                     param->ble_security.auth_cmpl.bd_addr[2],
                     param->ble_security.auth_cmpl.bd_addr[3],
                     param->ble_security.auth_cmpl.bd_addr[4],
                     param->ble_security.auth_cmpl.bd_addr[5]);
            ESP_LOGI(TAG, "connected to: %s", s_connected_addr);
            // Reuse the original per-slot key for notification/wheel state even
            // if authentication reports an identity alias or a fresh RPA.
            if (s_pairing_discoverable && s_pairing_new_keys)
                hid_peer_new_pairing(param->ble_security.auth_cmpl.bd_addr);
            hid_peer_authenticated(s_pairing_discoverable ? param->ble_security.auth_cmpl.bd_addr :
                                                           s_device_slots[s_selected_device_slot].addr);
            // CONFIG_BT_GATTS_SEND_SERVICE_CHANGE_AUTO handles notification of
            // the new feature characteristic; manual indication is forbidden.
            if (s_pairing_discoverable) {
                s_device_slots[s_selected_device_slot].valid = true;
                memcpy(s_device_slots[s_selected_device_slot].addr, param->ble_security.auth_cmpl.bd_addr, 6);
                s_device_slots[s_selected_device_slot].addr_type = (uint8_t)param->ble_security.auth_cmpl.addr_type;
                if (save_device_slot(s_selected_device_slot) != ESP_OK) {
                    memset(&s_device_slots[s_selected_device_slot], 0, sizeof(s_device_slots[0]));
                    s_connected = false;
                    s_allow_auto_advertise = false;
                    esp_ble_gap_disconnect(s_connected_bda);
                    break;
                }
                // A fresh key exchange explicitly repairs/moves an existing
                // host into this position. Retain its bond and all other peers.
                if (s_pairing_new_keys) {
                    for (uint8_t i = 0; i < BLE_HID_DEVICE_SLOT_COUNT; ++i) {
                        if (i == s_selected_device_slot ||
                            !address_matches_slot(i, param->ble_security.auth_cmpl.bd_addr)) continue;
                        ble_hid_device_slot_t previous = s_device_slots[i];
                        memset(&s_device_slots[i], 0, sizeof(s_device_slots[i]));
                        if (save_device_slot(i) != ESP_OK) {
                            s_device_slots[i] = previous;
                            ESP_LOGW(TAG, "could not clear previous device position %u", i + 1);
                        }
                    }
                }
                s_pairing_discoverable = false;
                // Keep the new encrypted HID link untouched. Configure the
                // next reconnect only after it is disconnected, outside GAP.
                s_filter_pending = true;
                s_allow_auto_advertise = false;
            }
        } else {
            s_allow_auto_advertise = false;
            esp_ble_gap_disconnect(param->ble_security.auth_cmpl.bd_addr);
            ESP_LOGI(TAG, "pairing failed; advertising paused");
        }
        break;

    default:
        break;
    }
}

static void hidd_event_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    switch (id) {
    case ESP_HIDD_START_EVENT:
        ESP_LOGI(TAG, "HID started");
        s_hid_started = true;
        break;

    case ESP_HIDD_CONNECT_EVENT:
        // Authentication, not the raw link, makes HID input available.
        s_mouse_retry_tick = xTaskGetTickCount() + pdMS_TO_TICKS(500);
        s_mouse_report_pending = false;
        s_advertising = false;
        s_next_battery_notify_tick = xTaskGetTickCount() + pdMS_TO_TICKS(BLE_HID_BATTERY_FIRST_NOTIFY_MS);
        ESP_LOGI(TAG, "connected");
        break;

    case ESP_HIDD_FEATURE_EVENT: {
        esp_hidd_event_data_t *event = event_data;
        if (event->feature.map_index == BLE_HID_REPORT_MAP_INDEX &&
            event->feature.report_id == BLE_HID_WHEEL_FEATURE_REPORT_ID &&
            event->feature.length == 1 && event->feature.data != NULL &&
            event->feature.data[0] <= 1) {
            ESP_LOGI(TAG, "wheel resolution negotiated: %u units/detent", hid_peer_wheel_resolution());
        }
        break;
    }

    case ESP_HIDD_DISCONNECT_EVENT:
        s_connected = false;
        s_mouse_retry_tick = 0;
        s_mouse_report_pending = false;
        s_advertising = false;
        s_next_battery_notify_tick = 0;
        memset(s_connected_bda, 0, sizeof(s_connected_bda));
        s_connected_addr[0] = '\0';
        ESP_LOGI(TAG, "disconnected: reason=0x%02x",
                 ((esp_hidd_event_data_t *)event_data)->disconnect.reason);
        s_link_up = false;
        start_advertising_if_ready();
        break;

    default:
        break;
    }
}

static void slot_gatts_callback(esp_gatts_cb_event_t event, esp_gatt_if_t interface,
                                esp_ble_gatts_cb_param_t *param)
{
    if (event == ESP_GATTS_CONNECT_EVT) {
        if (!s_link_up) s_pairing_new_keys = false;
        s_advertising = false;
        memcpy(s_connected_bda, param->connect.remote_bda, 6);
        s_link_up = true;
    }
    hid_peer_gatts_callback(event, interface, param);
}

static esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase failed");
        ret = nvs_flash_init();
    }
    return ret;
}

esp_err_t ble_hid_init(void)
{
    ESP_RETURN_ON_ERROR(init_nvs(), TAG, "nvs init failed");
    ESP_RETURN_ON_ERROR(device_names_init(), TAG, "load device names failed");
    load_device_slots();
    load_hid_mode();
    load_own_address();
    s_allow_auto_advertise = false;
    s_pairing_discoverable = false;

    esp_err_t event_ret = esp_event_loop_create_default();
    if (event_ret != ESP_OK && event_ret != ESP_ERR_INVALID_STATE) {
        return event_ret;
    }

    ESP_RETURN_ON_ERROR(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT), TAG, "release classic bt failed");

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_bt_controller_init(&bt_cfg), TAG, "bt controller init failed");
    ESP_RETURN_ON_ERROR(esp_bt_controller_enable(ESP_BT_MODE_BLE), TAG, "bt controller enable failed");
    ESP_RETURN_ON_ERROR(esp_bluedroid_init(), TAG, "bluedroid init failed");
    ESP_RETURN_ON_ERROR(esp_bluedroid_enable(), TAG, "bluedroid enable failed");
    if (s_have_random_addr) {
        ESP_RETURN_ON_ERROR(esp_ble_gap_set_rand_addr(s_random_addr),
                            TAG, "restore static random address failed");
    }

    ESP_RETURN_ON_ERROR(esp_ble_gap_register_callback(gap_event_handler), TAG, "gap callback failed");
    ESP_RETURN_ON_ERROR(esp_ble_gatts_register_callback(slot_gatts_callback), TAG, "gatts callback failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_device_name(DEVICE_NAME), TAG, "set device name failed");

    uint8_t auth_req = ESP_LE_AUTH_REQ_SC_BOND;
    uint8_t iocap = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req)), TAG, "auth param failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(iocap)), TAG, "iocap param failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size)), TAG, "key size param failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(init_key)), TAG, "init key param failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(rsp_key)), TAG, "rsp key param failed");

    ESP_RETURN_ON_ERROR(esp_ble_gap_config_adv_data(&s_adv_data), TAG, "adv data config failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_config_adv_data(&s_scan_rsp_data), TAG, "scan rsp config failed");
    ESP_RETURN_ON_ERROR(esp_hidd_dev_init(&s_hid_config, ESP_HID_TRANSPORT_BLE, hidd_event_callback, &s_hid_dev), TAG, "hid init failed");

    // Preserve the existing single-host pairing when upgrading old firmware.
    bool have_slot = false;
    for (unsigned i = 0; i < BLE_HID_DEVICE_SLOT_COUNT; ++i) have_slot |= s_device_slots[i].valid;
    int bond_count = esp_ble_get_bond_device_num();
    if (!have_slot && bond_count == 1) {
        esp_ble_bond_dev_t bond;
        if (esp_ble_get_bond_device_list(&bond_count, &bond) == ESP_OK) {
            s_device_slots[0].valid = true;
            memcpy(s_device_slots[0].addr, bond.bd_addr, 6);
            s_device_slots[0].addr_type = bond.bd_addr_type;
            ESP_RETURN_ON_ERROR(save_device_slot(0), TAG, "save migrated host failed");
        }
    }
    TickType_t start = xTaskGetTickCount();
    while (!s_hid_started) {
        if (xTaskGetTickCount() - start > pdMS_TO_TICKS(3000)) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_device_slots[s_selected_device_slot].valid)
        ESP_RETURN_ON_ERROR(ble_hid_connect_selected_device(), TAG, "restore selected host failed");

    int battery_mv = 0;
    if (battery_adc_read_actual_mv(&battery_mv) == ESP_OK) {
        s_last_battery_level = battery_adc_percent_from_mv(battery_mv);
    }

    if (s_battery_task == NULL) {
        BaseType_t task_ret = xTaskCreate(battery_notify_task, "battery_notify", BLE_HID_BATTERY_TASK_STACK, NULL, 5, &s_battery_task);
        if (task_ret != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

bool ble_hid_is_connected(void)
{
    return s_connected;
}

const char *ble_hid_get_connected_addr(void)
{
    return s_connected_addr[0] ? s_connected_addr : NULL;
}

static esp_err_t send_keyboard_report(uint8_t keycode)
{
    return ble_hid_send_key_combo(0, keycode);
}

esp_err_t ble_hid_send_key_combo(uint8_t modifier, uint8_t keycode)
{
    ESP_RETURN_ON_ERROR(ble_hid_keyboard_press(modifier, keycode), TAG, "key press failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    return ble_hid_keyboard_release();
}

esp_err_t ble_hid_keyboard_press(uint8_t modifier, uint8_t keycode)
{
    if (!s_connected || s_hid_dev == NULL) {
        return ESP_OK;
    }

    uint8_t report[8] = {0};
    report[0] = modifier;
    report[2] = keycode;

    ESP_LOGI(TAG, "keyboard modifier=0x%02x keycode=0x%02x", modifier, keycode);
    return esp_hidd_dev_input_set(s_hid_dev, BLE_HID_REPORT_MAP_INDEX, BLE_HID_KEYBOARD_REPORT_ID, report, sizeof(report));
}

esp_err_t ble_hid_keyboard_release(void)
{
    if (!s_connected || s_hid_dev == NULL) {
        return ESP_OK;
    }

    uint8_t report[8] = {0};
    return esp_hidd_dev_input_set(s_hid_dev, BLE_HID_REPORT_MAP_INDEX, BLE_HID_KEYBOARD_REPORT_ID, report, sizeof(report));
}

esp_err_t ble_hid_send_key(uint8_t keycode)
{
    if (keycode == 0) {
        return ESP_OK;
    }

    return send_keyboard_report(keycode);
}

esp_err_t ble_hid_send_consumer(uint16_t usage)
{
    if (usage == 0 || !s_connected || s_hid_dev == NULL) {
        return ESP_OK;
    }

    uint8_t report[2] = {
        (uint8_t)(usage & 0xFF),
        (uint8_t)(usage >> 8),
    };

    ESP_LOGI(TAG, "consumer usage=0x%04x", usage);
    ESP_RETURN_ON_ERROR(esp_hidd_dev_input_set(s_hid_dev, BLE_HID_REPORT_MAP_INDEX, BLE_HID_CONSUMER_REPORT_ID,
                                               report, sizeof(report)),
                        TAG, "consumer press failed");

    vTaskDelay(pdMS_TO_TICKS(20));
    memset(report, 0, sizeof(report));
    return esp_hidd_dev_input_set(s_hid_dev, BLE_HID_REPORT_MAP_INDEX, BLE_HID_CONSUMER_REPORT_ID,
                                  report, sizeof(report));
}

esp_err_t ble_hid_send_gamepad(uint16_t buttons, uint8_t hat)
{
    if (!s_full_hid_mode || !s_connected || s_hid_dev == NULL) {
        return ESP_OK;
    }

    if (hat > 8) {
        hat = 8;
    }

    uint8_t report[7] = {
        (uint8_t)(buttons & 0xFF),
        (uint8_t)(buttons >> 8),
        (uint8_t)(hat & 0x0F),
        0x80, // X neutral
        0x80, // Y neutral
        0x80, // Rx neutral
        0x80, // Ry neutral
    };

    ESP_LOGI(TAG, "gamepad buttons=0x%04x hat=%u", buttons, hat);
    return esp_hidd_dev_input_set(s_hid_dev, BLE_HID_REPORT_MAP_INDEX, BLE_HID_GAMEPAD_REPORT_ID,
                                  report, sizeof(report));
}

bool ble_hid_is_full_mode(void)
{
    return s_full_hid_mode;
}

esp_err_t ble_hid_toggle_hid_mode(void)
{
    bool next_full_mode = !s_full_hid_mode;
    ESP_LOGI(TAG, "switch HID mode to %s", next_full_mode ? "full" : "compatible");
    ESP_RETURN_ON_ERROR(save_hid_mode(next_full_mode), TAG, "save hid mode failed");
    ESP_RETURN_ON_ERROR(ble_hid_clear_pairing(), TAG, "clear pairing before mode switch failed");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

esp_err_t ble_hid_disconnect(void)
{
    s_filter_pending = false;
    s_allow_auto_advertise = false;
    s_pairing_discoverable = false;
    ESP_RETURN_ON_ERROR(stop_advertising_now(), TAG, "stop advertising failed");
    if (s_link_up) {
        ESP_RETURN_ON_ERROR(esp_ble_gap_disconnect(s_connected_bda), TAG, "disconnect failed");
        ESP_RETURN_ON_ERROR(wait_flag_clear(&s_link_up), TAG, "disconnect timeout");
    }
    return ESP_OK;
}

esp_err_t ble_hid_enable_connection(void)
{
    return ble_hid_connect_selected_device();
}

esp_err_t ble_hid_select_device_slot(uint8_t slot)
{
    if (slot >= BLE_HID_DEVICE_SLOT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t previous = s_selected_device_slot;
    s_selected_device_slot = slot;
    esp_err_t ret = save_selected_slot();
    if (ret != ESP_OK) { s_selected_device_slot = previous; return ret; }
    ESP_LOGI(TAG, "selected device slot %u", (unsigned)(slot + 1));
    return ESP_OK;
}

uint8_t ble_hid_get_selected_device_slot(void)
{
    return s_selected_device_slot;
}

bool ble_hid_get_device_slot(uint8_t slot, ble_hid_device_slot_t *out)
{
    if (slot >= BLE_HID_DEVICE_SLOT_COUNT || out == NULL) {
        return false;
    }

    *out = s_device_slots[slot];
    return s_device_slots[slot].valid;
}

bool ble_hid_is_pairing_mode(void)
{
    return s_pairing_discoverable;
}

static esp_err_t start_selected_slot(bool pairing)
{
    s_transition = true;
    esp_err_t ret = ble_hid_disconnect();
    if (ret == ESP_OK) ret = configure_slot_filter(pairing);
    if (ret == ESP_OK) {
        s_filter_pending = false;
        s_pairing_new_keys = false;
        s_pairing_discoverable = pairing;
        s_allow_auto_advertise = true;
        ret = configure_discoverable(pairing);
    }
    if (ret != ESP_OK) s_allow_auto_advertise = false;
    s_transition = false;
    start_advertising_if_ready();
    if (ret == ESP_OK) ESP_LOGI(TAG, "device %u: %s", s_selected_device_slot + 1, pairing ? "pairing" : "reconnecting selected host");
    return ret;
}

esp_err_t ble_hid_connect_selected_device(void)
{
    if (!s_device_slots[s_selected_device_slot].valid) return ESP_ERR_NOT_FOUND;
    return start_selected_slot(false);
}

void ble_hid_poll(void)
{
    // Main-loop context serializes this with UI device switches. Never call
    // blocking controller operations from the GAP callback or a second task.
    if (!s_filter_pending || s_link_up || s_transition) return;
    s_filter_pending = false;
    esp_err_t ret = ble_hid_connect_selected_device();
    if (ret != ESP_OK) ESP_LOGE(TAG, "deferred reconnect failed: %s", esp_err_to_name(ret));
}

esp_err_t ble_hid_switch_device_slot(uint8_t slot)
{
    if (slot >= BLE_HID_DEVICE_SLOT_COUNT) return ESP_ERR_INVALID_ARG;
    if (slot == s_selected_device_slot && s_connected) return ESP_OK;
    // Disconnect while the old slot is still selected; no late auth may bind
    // the old host into the newly selected slot.
    s_transition = true;
    esp_err_t ret = ble_hid_disconnect();
    if (ret == ESP_OK) ret = ble_hid_select_device_slot(slot);
    s_transition = false;
    if (ret != ESP_OK) return ret;
    return start_selected_slot(!s_device_slots[slot].valid);
}

esp_err_t ble_hid_enter_pairing_mode(void)
{
    s_transition = true;
    esp_err_t ret = ble_hid_disconnect();
    if (ret == ESP_OK && s_device_slots[s_selected_device_slot].valid) {
        remove_bond_for_addr(s_device_slots[s_selected_device_slot].addr);
        memset(&s_device_slots[s_selected_device_slot], 0, sizeof(s_device_slots[0]));
        ret = save_device_slot(s_selected_device_slot);
    }
    s_transition = false;
    if (ret != ESP_OK) return ret;
    // Keep the peripheral identity and the other two computers' keys intact.
    return start_selected_slot(true);
}

esp_err_t ble_hid_pair_selected_device(void)
{
    return ble_hid_enter_pairing_mode();
}

esp_err_t ble_hid_clear_pairing(void)
{
    int dev_num = esp_ble_get_bond_device_num();
    ESP_LOGI(TAG, "clear pairing, bonded devices=%d", dev_num);

    s_allow_auto_advertise = false;
    s_pairing_discoverable = false;
    ESP_RETURN_ON_ERROR(ble_hid_disconnect(), TAG, "disconnect before clear failed");

    if (dev_num > 0) {
        esp_ble_bond_dev_t bonds[dev_num];
        ESP_RETURN_ON_ERROR(esp_ble_get_bond_device_list(&dev_num, bonds), TAG, "get bond list failed");

        for (int i = 0; i < dev_num; i++) {
            hid_peer_forget(bonds[i].bd_addr);
            esp_err_t ret = esp_ble_remove_bond_device(bonds[i].bd_addr);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "remove bond[%d] failed: %s", i, esp_err_to_name(ret));
            }
        }
    }

    ESP_RETURN_ON_ERROR(stop_advertising_now(), TAG, "stop before clear failed");
    memset(s_device_slots, 0, sizeof(s_device_slots));
    s_selected_device_slot = 0;
    ESP_RETURN_ON_ERROR(save_selected_slot(), TAG, "save reset selection failed");
    for (uint8_t i = 0; i < BLE_HID_DEVICE_SLOT_COUNT; i++) {
        ESP_RETURN_ON_ERROR(save_device_slot(i), TAG, "save reset slot failed");
    }
    ESP_LOGI(TAG, "pairing memory cleared");
    return ESP_OK;
}

uint16_t ble_hid_get_wheel_resolution(void)
{
    return hid_peer_wheel_resolution();
}

bool ble_hid_mouse_report_ready(void)
{
    return s_connected && hid_peer_ready() && !s_mouse_report_pending && s_mouse_retry_tick == 0;
}

esp_err_t ble_hid_send_mouse_report_precise(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel)
{
    if (!s_connected || s_hid_dev == NULL || !hid_peer_ready()) {
        ESP_LOGD(TAG, "mouse report dropped (not connected)");
        return ESP_OK;
    }
    // Connection arrives before the host subscribes to input notifications.
    // Give discovery time, and retry transient send failures without aborting
    // the main loop (especially when booting directly into mouse mode).
    TickType_t now = xTaskGetTickCount();
    if (s_mouse_retry_tick != 0 && (int32_t)(now - s_mouse_retry_tick) < 0) return ESP_OK;

    uint8_t report[4] = {buttons, (uint8_t)dx, (uint8_t)dy, (uint8_t)wheel};

    static TickType_t diagnostic_tick;
    if (now - diagnostic_tick >= pdMS_TO_TICKS(1000)) {
        diagnostic_tick = now;
        ESP_LOGI(TAG, "mouse diagnostic buttons=%u dx=%d dy=%d wheel=%d", buttons, dx, dy, wheel);
    }
    esp_err_t ret = esp_hidd_dev_input_set(s_hid_dev, BLE_HID_REPORT_MAP_INDEX, BLE_HID_MOUSE_REPORT_ID, report, sizeof(report));
    if (ret != ESP_OK) {
        if (!s_mouse_report_pending) {
            ESP_LOGW(TAG, "mouse notifications not ready; waiting for host: %s", esp_err_to_name(ret));
        }
        s_mouse_report_pending = true;
        s_mouse_retry_tick = now + pdMS_TO_TICKS(200);
        return ESP_OK;
    }
    if (s_mouse_report_pending) ESP_LOGI(TAG, "mouse notifications ready");
    s_mouse_report_pending = false;
    s_mouse_retry_tick = 0;
    return ret;
}

esp_err_t ble_hid_send_mouse_report(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel)
{
    // Existing gesture/custom-mode callers still specify whole wheel detents.
    int32_t remaining = (int32_t)wheel * ble_hid_get_wheel_resolution();
    do {
        int8_t step = remaining > 127 ? 127 : remaining < -127 ? -127 : (int8_t)remaining;
        ESP_RETURN_ON_ERROR(ble_hid_send_mouse_report_precise(buttons, dx, dy, step),
                            TAG, "wheel report failed");
        remaining -= step;
        dx = dy = 0;
    } while (remaining != 0);
    return ESP_OK;
}

esp_err_t ble_hid_drag_vertical(bool upward)
{
    int8_t wheel = upward ? -1 : 1;

    ESP_LOGI(TAG, "scroll %s start (%d steps, wheel=%d)", upward ? "up" : "down", BLE_HID_DRAG_STEPS, wheel);

    // Define motion pipeline: corner → center → scroll → reset
    const mouse_step_t steps[] = {
        // ① fly to top-left corner
        {MOUSE_STEP_MOVE,   0, -100, -100, 0,  10, 0},
        {MOUSE_STEP_RESET,  0,  0,   0,   0,   1, 30},
        // ② move toward center
        {MOUSE_STEP_MOVE,   0,  0,   60,  0,   1, 0},
        {MOUSE_STEP_RESET,  0,  0,   0,   0,   1, 30},
        // ③ scroll
        {MOUSE_STEP_SCROLL, 0,  0,   0,   wheel, BLE_HID_DRAG_STEPS, 0},
        // ④ reset
        {MOUSE_STEP_RESET,  0,  0,   0,   0,   1, 0},
    };

    return ble_hid_mouse_exec(steps, sizeof(steps) / sizeof(steps[0]));
}

esp_err_t ble_hid_mouse_exec(const mouse_step_t *steps, size_t count)
{
    if (!s_connected || s_hid_dev == NULL) {
        ESP_LOGW(TAG, "mouse exec dropped (not connected)");
        return ESP_OK;
    }

    for (size_t i = 0; i < count; i++) {
        const mouse_step_t *s = &steps[i];

        switch (s->type) {
        case MOUSE_STEP_MOVE:
            for (uint8_t r = 0; r < s->repeat; r++) {
                ESP_RETURN_ON_ERROR(ble_hid_send_mouse_report(s->buttons, s->dx, s->dy, 0),
                                    TAG, "move step[%u] failed", (unsigned)i);
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            break;

        case MOUSE_STEP_SCROLL:
            for (uint8_t r = 0; r < s->repeat; r++) {
                ESP_RETURN_ON_ERROR(ble_hid_send_mouse_report(s->buttons, 0, 0, s->wheel),
                                    TAG, "scroll step[%u] failed", (unsigned)i);
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            break;

        case MOUSE_STEP_RESET:
            ESP_RETURN_ON_ERROR(ble_hid_send_mouse_report(0, 0, 0, 0),
                                TAG, "reset step[%u] failed", (unsigned)i);
            break;

        case MOUSE_STEP_DELAY:
            vTaskDelay(pdMS_TO_TICKS(s->delay_ms));
            break;

        default:
            break;
        }

        if (s->delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(s->delay_ms));
        }
    }

    return ESP_OK;
}
