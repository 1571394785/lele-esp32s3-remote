#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_gatts_api.h"

void hid_peer_gatts_callback(esp_gatts_cb_event_t event, esp_gatt_if_t interface,
                             esp_ble_gatts_cb_param_t *param);
void hid_peer_new_pairing(const uint8_t address[6]);
void hid_peer_authenticated(const uint8_t address[6]);
void hid_peer_forget(const uint8_t address[6]);
uint16_t hid_peer_wheel_resolution(void);
bool hid_peer_ready(void);
