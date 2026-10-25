// SPDX-License-Identifier: GPL-3.0-or-later
// Host-test stub: there is no NVS on the host; every access fails cleanly.
#pragma once
#include "esp_err.h"
inline esp_err_t nvs_flash_init() { return ESP_FAIL; }
inline esp_err_t nvs_flash_erase() { return ESP_FAIL; }
