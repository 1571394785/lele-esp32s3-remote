#pragma once
#include <stdbool.h>
#include <stdint.h>
// ESP-IDF stores SMP keys little-endian; addresses use display/network order.
bool ble_peer_rpa_matches(const uint8_t irk[16], const uint8_t address[6]);
