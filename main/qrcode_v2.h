#pragma once

#include <stdbool.h>

#include "esp_err.h"

#define QR_V2_SIZE 21
#define QR_V2_MAX_BYTES 20

esp_err_t qrcode_v2_make(const char *text, bool modules[QR_V2_SIZE][QR_V2_SIZE]);
