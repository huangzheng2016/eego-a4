// SPDX-License-Identifier: GPL-3.0-or-later
// 触控标定 NVS 持久化（见 touch_calibration_store.h）。
#include "touch_calibration_store.h"

#include "nvs.h"
#include "nvs_flash.h"

namespace eego::touchcal {

namespace {

// 触控在 NVS 初始化之前启动（board 启动合同）：这里幂等地确保 NVS 可用。
// 分区需擦除等异常情况留给组合根（CredentialStore::ensureNvs）处理，此处只读失败。
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
