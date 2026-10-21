// SPDX-License-Identifier: GPL-3.0-or-later
// NVS 凭据存储实现（wifi/sec 命名空间，自管读写）。
#include "net_stack/credentials.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace eego::net {

static const char* TAG = "net.cred";
static const char* kNsWifi = "wifi";
static const char* kNsSec = "sec";
static const char* kKeyCount = "count";
constexpr size_t kMaxWifiAccounts = 8;  // 与 provision 门户/admin 配置上限对齐

esp_err_t CredentialStore::ensureNvs() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(TAG, "nvs partition needs erase, reinitializing");
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_flash_erase());
    err = nvs_flash_init();
  }
  if (err == ESP_ERR_INVALID_STATE) return ESP_OK;  // 已被其他组件初始化
  if (err != ESP_OK) ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
  return err;
}

size_t CredentialStore::loadWifiAccounts(std::vector<WifiAccount>& out) {
  out.clear();
  if (ensureNvs() != ESP_OK) return 0;
  nvs_handle_t h;
  if (nvs_open(kNsWifi, NVS_READONLY, &h) != ESP_OK) return 0;  // 命名空间不存在 = 未配网

  uint8_t count = 0;
  nvs_get_u8(h, kKeyCount, &count);
  if (count > kMaxWifiAccounts) count = kMaxWifiAccounts;

  for (uint8_t i = 0; i < count; i++) {
    char key[16];
    snprintf(key, sizeof(key), "ssid%u", (unsigned)i);
    size_t len = 0;
    if (nvs_get_str(h, key, nullptr, &len) != ESP_OK || len == 0 || len > 33) continue;
    WifiAccount acc;
    acc.ssid.resize(len - 1);
    if (nvs_get_str(h, key, acc.ssid.data(), &len) != ESP_OK || acc.ssid.empty()) continue;
    snprintf(key, sizeof(key), "pass%u", (unsigned)i);
    len = 0;
    if (nvs_get_str(h, key, nullptr, &len) == ESP_OK && len > 0 && len <= 65) {
      acc.password.resize(len - 1);
      if (nvs_get_str(h, key, acc.password.data(), &len) != ESP_OK) acc.password.clear();
    }
    out.push_back(std::move(acc));
  }
  nvs_close(h);
  return out.size();
}

esp_err_t CredentialStore::saveWifiAccounts(const std::vector<WifiAccount>& in) {
  if (ensureNvs() != ESP_OK) return ESP_FAIL;
  if (in.size() > kMaxWifiAccounts) return ESP_ERR_INVALID_ARG;
  nvs_handle_t h;
  esp_err_t err = nvs_open(kNsWifi, NVS_READWRITE, &h);
  if (err != ESP_OK) return err;

  err = nvs_erase_all(h);  // 整命名空间重写，避免残留旧索引
  if (err == ESP_OK) {
    for (size_t i = 0; i < in.size() && err == ESP_OK; i++) {
      char key[16];
      snprintf(key, sizeof(key), "ssid%u", (unsigned)i);
      err = nvs_set_str(h, key, in[i].ssid.c_str());
      if (err != ESP_OK) break;
      snprintf(key, sizeof(key), "pass%u", (unsigned)i);
      err = nvs_set_str(h, key, in[i].password.c_str());
    }
  }
  if (err == ESP_OK) err = nvs_set_u8(h, kKeyCount, (uint8_t)in.size());
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  // 只记条数，不记任何凭据内容（M13 日志边界）
  if (err == ESP_OK) ESP_LOGI(TAG, "wifi accounts saved: %u", (unsigned)in.size());
  else ESP_LOGE(TAG, "wifi accounts save failed: %s", esp_err_to_name(err));
  return err;
}

esp_err_t CredentialStore::clearWifiAccounts() {
  if (ensureNvs() != ESP_OK) return ESP_FAIL;
  return clearNamespace(kNsWifi);
}

std::string CredentialStore::resolveToken(const std::string& ref) {
  if (ref.compare(0, 4, "nvs:") != 0 || ref.size() <= 4) return {};
  std::string body = ref.substr(4);
  std::string ns = kNsSec;
  std::string key = body;
  size_t slash = body.find('/');
  if (slash != std::string::npos) {
    ns = body.substr(0, slash);
    key = body.substr(slash + 1);
  }
  if (ns.empty() || key.empty() || ns.size() > 15 || key.size() > 31) return {};
  if (ensureNvs() != ESP_OK) return {};

  nvs_handle_t h;
  if (nvs_open(ns.c_str(), NVS_READONLY, &h) != ESP_OK) return {};
  size_t len = 0;
  std::string value;
  if (nvs_get_str(h, key.c_str(), nullptr, &len) == ESP_OK && len > 0 && len <= 1025) {
    value.resize(len - 1);
    if (nvs_get_str(h, key.c_str(), value.data(), &len) != ESP_OK) value.clear();
  }
  nvs_close(h);
  return value;
}

esp_err_t CredentialStore::saveToken(const std::string& ns, const std::string& key,
                                     const std::string& value) {
  if (ensureNvs() != ESP_OK) return ESP_FAIL;
  if (ns.empty() || key.empty() || ns.size() > 15 || key.size() > 31) {
    return ESP_ERR_INVALID_ARG;
  }
  nvs_handle_t h;
  esp_err_t err = nvs_open(ns.c_str(), NVS_READWRITE, &h);
  if (err != ESP_OK) return err;
  err = nvs_set_str(h, key.c_str(), value.c_str());
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  return err;
}

esp_err_t CredentialStore::clearNamespace(const std::string& ns) {
  if (ensureNvs() != ESP_OK) return ESP_FAIL;
  if (ns.empty() || ns.size() > 15) return ESP_ERR_INVALID_ARG;
  nvs_handle_t h;
  esp_err_t err = nvs_open(ns.c_str(), NVS_READWRITE, &h);
  if (err != ESP_OK) return err;
  err = nvs_erase_all(h);
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  return err;
}

std::string CredentialStore::describeRef(const std::string& ref) {
  if (ref.compare(0, 4, "nvs:") != 0 || ref.size() <= 4) return "(invalid ref)";
  std::string body = ref.substr(4);
  size_t slash = body.find('/');
  if (slash == std::string::npos) return "nvs:sec/" + body;
  return "nvs:" + body;
}

}  // namespace eego::net
