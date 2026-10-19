#include "hid_peer_state.h"

#include <stdio.h>
#include <string.h>
#include "air_mouse_scroll.h"
#include "ble_hid.h"
#include "esp_hidd_gatts.h"
#include "esp_log.h"
#include "nvs.h"

// Persist host-written CCCDs by service/attribute index, never by raw handles.
// Bluedroid's HID implementation only retains its notification flags in RAM.
#define CCC_COUNT 8
#define TABLE_COUNT 2
#define STATE_VERSION 1
typedef struct {
    uint8_t version, feature, feature_valid, reserved;
    uint16_t known;
    uint16_t ccc[CCC_COUNT];
    uint32_t schema;
} peer_record_t;
typedef struct {
    esp_gatt_if_t interface;
    uint16_t count;
    uint16_t indices[CCC_COUNT];
    uint8_t slots[CCC_COUNT];
    uint8_t used;
    uint16_t feature_index;
} table_t;
static table_t s_tables[TABLE_COUNT];
static uint16_t s_handles[CCC_COUNT];
static esp_gatt_if_t s_interfaces[CCC_COUNT];
static uint16_t s_feature_handle;
static uint16_t s_connection;
static uint8_t s_address[6];
static bool s_active, s_authenticated;
static uint16_t s_written;
static bool s_feature_written;
static peer_record_t s_record;
static uint32_t s_schema;
static const char *TAG = "hid_peer";

static void peer_key(const uint8_t address[6], char key[14])
{
    snprintf(key, 14, "p%02x%02x%02x%02x%02x%02x", address[0], address[1],
             address[2], address[3], address[4], address[5]);
}

static void save_record(void)
{
    if (!s_active || !s_authenticated) return;
    char key[14];
    peer_key(s_address, key);
    nvs_handle_t nvs;
    esp_err_t result = nvs_open("hidpeer", NVS_READWRITE, &nvs);
    if (result != ESP_OK) { ESP_LOGW(TAG, "open state failed: %s", esp_err_to_name(result)); return; }
    result = nvs_set_blob(nvs, key, &s_record, sizeof(s_record));
    if (result == ESP_OK) result = nvs_commit(nvs);
    nvs_close(nvs);
    if (result != ESP_OK) ESP_LOGW(TAG, "save state failed: %s", esp_err_to_name(result));
}

static void load_record(void)
{
    memset(&s_record, 0, sizeof(s_record));
    char key[14];
    peer_key(s_address, key);
    nvs_handle_t nvs;
    if (nvs_open("hidpeer", NVS_READONLY, &nvs) == ESP_OK) {
        size_t size = sizeof(s_record);
        esp_err_t result = nvs_get_blob(nvs, key, &s_record, &size);
        nvs_close(nvs);
        if (result != ESP_OK || size != sizeof(s_record) || s_record.version != STATE_VERSION ||
            s_record.schema != s_schema || s_record.feature > 1) memset(&s_record, 0, sizeof(s_record));
    }
    s_record.version = STATE_VERSION;
    s_record.schema = s_schema;
}

void hid_peer_forget(const uint8_t address[6])
{
    char key[14];
    peer_key(address, key);
    nvs_handle_t nvs;
    if (nvs_open("hidpeer", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_key(nvs, key);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

// Observe the real attribute database so both boot/report mode CCCDs remain
// correct if the HID map changes. No private ESP-IDF structs are accessed.
esp_err_t __real_esp_ble_gatts_create_attr_tab(const esp_gatts_attr_db_t *,
                                            esp_gatt_if_t, uint16_t, uint8_t);
esp_err_t __wrap_esp_ble_gatts_create_attr_tab(const esp_gatts_attr_db_t *db,
                                            esp_gatt_if_t interface,
                                            uint16_t count, uint8_t instance)
{
    uint16_t uuid = 0;
    if (count && db[0].att_desc.length == 2 && db[0].att_desc.value)
        memcpy(&uuid, db[0].att_desc.value, 2);
    int table_index = uuid == 0x180f ? 0 : uuid == 0x1812 && instance == 0 ? 1 : -1;
    if (table_index >= 0) {
        table_t *table = &s_tables[table_index];
        memset(table, 0, sizeof(*table));
        table->interface = interface;
        table->count = count;
        table->feature_index = UINT16_MAX;
        s_schema = s_schema * 16777619u ^ ((uint32_t)uuid << 16 | count);
        for (uint16_t i = 0; i < count; ++i) {
            uint16_t attribute_uuid = 0;
            if (db[i].att_desc.uuid_length == 2)
                memcpy(&attribute_uuid, db[i].att_desc.uuid_p, 2);
            s_schema = s_schema * 16777619u ^ attribute_uuid;
            if (attribute_uuid == 0x2908 && db[i].att_desc.length == 2 && db[i].att_desc.value)
                s_schema = s_schema * 16777619u ^ (db[i].att_desc.value[0] | db[i].att_desc.value[1] << 8);
            if (attribute_uuid == 0x2902 && table->used < (table_index ? CCC_COUNT - 1 : 1)) {
                uint8_t item = table->used++;
                table->indices[item] = i;
                table->slots[item] = table_index ? item + 1 : 0;
            }
            if (attribute_uuid == 0x2908 && db[i].att_desc.length == 2 &&
                db[i].att_desc.value && db[i].att_desc.value[0] == BLE_HID_WHEEL_FEATURE_REPORT_ID &&
                db[i].att_desc.value[1] == 3 && i > 0) table->feature_index = i - 1;
        }
    }
    return __real_esp_ble_gatts_create_attr_tab(db, interface, count, instance);
}

static void apply_ccc(unsigned slot, uint16_t value)
{
    if (!s_handles[slot]) return;
    uint8_t bytes[2] = {value & 0xff, value >> 8};
    esp_ble_gatts_cb_param_t write = {0};
    write.write.conn_id = s_connection;
    memcpy(write.write.bda, s_address, 6);
    write.write.handle = s_handles[slot];
    write.write.len = sizeof(bytes);
    write.write.value = bytes;
    // Replay only the library's local CCC update; never send a host write or
    // bypass authentication. The original callback has no public CCC setter.
    esp_hidd_gatts_event_handler(ESP_GATTS_WRITE_EVT, s_interfaces[slot], &write);
    esp_ble_gatts_set_attr_value(s_handles[slot], sizeof(bytes), bytes);
}

void hid_peer_new_pairing(const uint8_t address[6])
{
    if (!s_active) return;
    peer_record_t fresh = s_record;
    memset(&s_record, 0, sizeof(s_record));
    s_record.version = STATE_VERSION;
    s_record.schema = s_schema;
    s_record.known = s_written;
    for (unsigned i = 0; i < CCC_COUNT; ++i)
        if (s_written & (1u << i)) s_record.ccc[i] = fresh.ccc[i];
    if (s_feature_written) {
        s_record.feature = fresh.feature;
        s_record.feature_valid = fresh.feature_valid;
    }
    memcpy(s_address, address, 6);
}

void hid_peer_authenticated(const uint8_t address[6])
{
    if (!s_active) return;
    // Authentication resolves a private connection address to its bonded
    // identity. Keep this connection's fresh writes when switching NVS keys.
    if (memcmp(address, s_address, 6) != 0) {
        peer_record_t fresh = s_record;
        memcpy(s_address, address, 6);
        load_record();
        for (unsigned i = 0; i < CCC_COUNT; ++i)
            if (s_written & (1u << i)) s_record.ccc[i] = fresh.ccc[i];
        s_record.known |= s_written;
        if (s_feature_written) { s_record.feature = fresh.feature; s_record.feature_valid = 1; }
    }
    s_authenticated = true;
    unsigned restored = 0;
    for (unsigned i = 0; i < CCC_COUNT; ++i) {
        if (s_record.known & (1u << i)) { apply_ccc(i, s_record.ccc[i]); ++restored; }
    }
    if (s_feature_handle) esp_ble_gatts_set_attr_value(s_feature_handle, 1, &s_record.feature);
    save_record(); // Includes valid writes that arrived during authentication.
    ESP_LOGI(TAG, "authenticated; restored %u subscriptions, wheel=%u", restored,
             hid_peer_wheel_resolution());
}

uint16_t hid_peer_wheel_resolution(void)
{
    return s_record.feature_valid && s_record.feature ? AIR_MOUSE_WHEEL_RESOLUTION : 1;
}

bool hid_peer_ready(void) { return s_active && s_authenticated; }

void hid_peer_gatts_callback(esp_gatts_cb_event_t event, esp_gatt_if_t interface,
                            esp_ble_gatts_cb_param_t *param)
{
    if (event == ESP_GATTS_CREAT_ATTR_TAB_EVT && param->add_attr_tab.status == ESP_GATT_OK) {
        for (unsigned t = 0; t < TABLE_COUNT; ++t) {
            table_t *table = &s_tables[t];
            if (table->interface != interface || table->count != param->add_attr_tab.num_handle) continue;
            for (unsigned i = 0; i < table->used; ++i) {
                unsigned slot = table->slots[i];
                s_handles[slot] = param->add_attr_tab.handles[table->indices[i]];
                s_interfaces[slot] = interface;
            }
            if (table->feature_index != UINT16_MAX)
                s_feature_handle = param->add_attr_tab.handles[table->feature_index];
        }
    }
    if (event == ESP_GATTS_CONNECT_EVT && !s_active) {
        s_active = true;
        s_authenticated = false;
        s_connection = param->connect.conn_id;
        memcpy(s_address, param->connect.remote_bda, 6);
        s_written = 0;
        s_feature_written = false;
        load_record();
        for (unsigned i = 0; i < CCC_COUNT; ++i) apply_ccc(i, 0);
        if (s_feature_handle) esp_ble_gatts_set_attr_value(s_feature_handle, 1, &s_record.feature);
    }
    if (event == ESP_GATTS_DISCONNECT_EVT) { s_active = false; s_authenticated = false; }
    if (event == ESP_GATTS_WRITE_EVT && s_active && param->write.conn_id == s_connection &&
        !param->write.is_prep && param->write.offset == 0 && param->write.value) {
        bool changed = false;
        for (unsigned i = 0; i < CCC_COUNT; ++i) {
            if (param->write.handle != s_handles[i] || param->write.len != 2) continue;
            uint16_t value = param->write.value[0] | (uint16_t)param->write.value[1] << 8;
            if (value > 3) continue;
            changed = !(s_record.known & (1u << i)) || s_record.ccc[i] != value;
            s_record.known |= 1u << i;
            s_written |= 1u << i;
            s_record.ccc[i] = value;
            ESP_LOGI(TAG, "host subscription %u=%u", i, value);
        }
        if (param->write.handle == s_feature_handle && param->write.len == 1 && param->write.value[0] <= 1) {
            changed = !s_record.feature_valid || s_record.feature != param->write.value[0];
            s_record.feature = param->write.value[0];
            s_record.feature_valid = 1;
            s_feature_written = true;
            ESP_LOGI(TAG, "host wheel resolution=%u", hid_peer_wheel_resolution());
        }
        if (changed) save_record();
    }
    esp_hidd_gatts_event_handler(event, interface, param);
}
