// SPDX-License-Identifier: GPL-3.0-or-later
// Host-test stub: there is no NVS on the host; every access fails cleanly.
#pragma once
#include <cstddef>
#include <cstdint>
#include "esp_err.h"

typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
typedef enum { NVS_TYPE_STR = 0x21, NVS_TYPE_ANY = 0xff } nvs_type_t;
typedef struct nvs_opaque_iterator_t* nvs_iterator_t;
typedef struct {
  char namespace_name[16];
  char key[16];
  nvs_type_t type;
} nvs_entry_info_t;

inline esp_err_t nvs_open(const char*, nvs_open_mode_t, nvs_handle_t*) { return ESP_ERR_NVS_NOT_FOUND; }
inline void nvs_close(nvs_handle_t) {}
inline esp_err_t nvs_commit(nvs_handle_t) { return ESP_FAIL; }
inline esp_err_t nvs_get_str(nvs_handle_t, const char*, char*, size_t*) { return ESP_ERR_NVS_NOT_FOUND; }
inline esp_err_t nvs_get_u8(nvs_handle_t, const char*, uint8_t*) { return ESP_ERR_NVS_NOT_FOUND; }
inline esp_err_t nvs_set_u8(nvs_handle_t, const char*, uint8_t) { return ESP_FAIL; }
inline esp_err_t nvs_set_str(nvs_handle_t, const char*, const char*) { return ESP_FAIL; }
inline esp_err_t nvs_erase_key(nvs_handle_t, const char*) { return ESP_ERR_NVS_NOT_FOUND; }
inline esp_err_t nvs_erase_all(nvs_handle_t) { return ESP_FAIL; }
inline esp_err_t nvs_entry_find(const char*, const char*, nvs_type_t, nvs_iterator_t*) {
  return ESP_ERR_NVS_NOT_FOUND;
}
inline esp_err_t nvs_entry_info(nvs_iterator_t, nvs_entry_info_t*) { return ESP_FAIL; }
inline esp_err_t nvs_entry_next(nvs_iterator_t*) { return ESP_ERR_NVS_NOT_FOUND; }
inline void nvs_release_iterator(nvs_iterator_t) {}
