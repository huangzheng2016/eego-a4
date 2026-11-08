// SPDX-License-Identifier: GPL-3.0-or-later
#include "touch_calibration_store.h"

#include "nvs.h"
#include "nvs_flash.h"

namespace eego::touchcal {
namespace {
esp_err_t ensureNvs() {
  const esp_err_t err = nvs_flash_init();
  return err == ESP_ERR_INVALID_STATE ? ESP_OK : err;
}

}  // namespace

esp_err_t loadPersisted(Affine& out) {
  esp_err_t err = ensureNvs();
  if (err != ESP_OK) return err;
  nvs_handle_t h;
  err = nvs_open(kNvsNamespace, NVS_READONLY, &h);
  if (err != ESP_OK) return err;
  uint8_t blob[kBlobSize] = {};
  size_t len = sizeof(blob);
  err = nvs_get_blob(h, kNvsKey, blob, &len);
  nvs_close(h);
  if (err != ESP_OK) return err;
  Affine decoded;
  if (!decode(blob, len, decoded)) return ESP_ERR_INVALID_CRC;
  out = decoded;
  return ESP_OK;
}

esp_err_t savePersisted(const Affine& affine) {
  if (!plausible(affine)) return ESP_ERR_INVALID_ARG;
  esp_err_t err = ensureNvs();
  if (err != ESP_OK) return err;
  uint8_t blob[kBlobSize];
  encode(affine, blob);
  nvs_handle_t h;
  err = nvs_open(kNvsNamespace, NVS_READWRITE, &h);
  if (err != ESP_OK) return err;
  err = nvs_set_blob(h, kNvsKey, blob, sizeof(blob));
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  return err;
}

esp_err_t erasePersisted() {
  esp_err_t err = ensureNvs();
  if (err != ESP_OK) return err;
  nvs_handle_t h;
  err = nvs_open(kNvsNamespace, NVS_READWRITE, &h);
  if (err != ESP_OK) return err;
  err = nvs_erase_all(h);
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  return err;
}

}  // namespace eego::touchcal
