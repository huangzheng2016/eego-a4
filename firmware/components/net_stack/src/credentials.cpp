// SPDX-License-Identifier: GPL-3.0-or-later
// NVS 凭据存储实现（wifi/sec/source 命名空间，自管读写）。
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

namespace {

std::string lowerAscii(const std::string& text) {
  std::string out = text;
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

bool reservedSourceKey(const std::string& key) {
  const std::string lower = lowerAscii(key);
  if (lower == "pw_hash" || lower == "ingest_key" || lower == "count" ||
      lower == "wifi" || lower == "ble" || lower == "nimble_bond" || lower == "sec") {
    return true;
  }
  const auto indexed = [&lower](const char* prefix) {
    const std::string p(prefix);
    if (lower.rfind(p, 0) != 0 || lower.size() == p.size()) return false;
    for (size_t i = p.size(); i < lower.size(); ++i) {
      if (lower[i] < '0' || lower[i] > '9') return false;
    }
    return true;
  };
  return indexed("pass") || indexed("ssid");
}

}  // namespace

bool CredentialStore::validSourceKey(const std::string& key) {
  if (key.empty() || key.size() > kSourceKeyMaxBytes) return false;
  for (const unsigned char c : key) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-')) {
      return false;
    }
  }
  return true;
}

bool CredentialStore::validSourceKeyForWrite(const std::string& key) {
  return validSourceKey(key) && !reservedSourceKey(key);
}

bool CredentialStore::safeHeaderValue(const std::string& value) {
  for (const unsigned char c : value) {
    if (c != '\t' && (c < 0x20 || c == 0x7f)) return false;
  }
  return true;
}

bool CredentialStore::validSourceValue(const std::string& value) {
  if (value.empty() || value.size() > kSourceValueMaxBytes) return false;
  for (const unsigned char c : value) {
    if (c < 0x20 || c == 0x7f) return false;
  }
  return true;
}

bool CredentialStore::validHeaderName(const std::string& name) {
  if (name.empty() || name.size() > 64) return false;
  static const std::string extra = "!#$%&'*+.^_`|~-";
  for (const unsigned char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || extra.find(static_cast<char>(c)) != std::string::npos)) {
      return false;
    }
  }
  return true;
}

bool CredentialStore::sensitiveHeaderName(const std::string& name) {
  const std::string lower = lowerAscii(name);
  return lower == "authorization" || lower == "proxy-authorization" ||
         lower == "cookie" || lower == "set-cookie" || lower == "x-api-key" ||
         lower.find("auth") != std::string::npos || lower.find("key") != std::string::npos ||
         lower.find("token") != std::string::npos || lower.find("pass") != std::string::npos ||
         lower.find("secret") != std::string::npos || lower.find("cookie") != std::string::npos;
}

std::string CredentialStore::sourceRef(const std::string& key) {
  if (!validSourceKey(key)) return {};
  return "nvs:source/" + key;
}

std::string CredentialStore::sourceKey(const std::string& ref) {
  static const std::string prefix = "nvs:source/";
  if (ref.rfind(prefix, 0) != 0) return {};
  const std::string key = ref.substr(prefix.size());
  return validSourceKey(key) ? key : std::string();
}

bool CredentialStore::validSourceRef(const std::string& ref) {
  return !sourceKey(ref).empty();
}

esp_err_t CredentialStore::saveSourceCredential(const std::string& key,
                                                const std::string& value) {
  if (!validSourceKeyForWrite(key) || !validSourceValue(value)) return ESP_ERR_INVALID_ARG;
  return saveToken(kSourceNamespace, key, value);
}

esp_err_t CredentialStore::deleteSourceCredential(const std::string& key) {
  if (!validSourceKeyForWrite(key)) return ESP_ERR_INVALID_ARG;
  if (ensureNvs() != ESP_OK) return ESP_FAIL;
  nvs_handle_t h;
  esp_err_t err = nvs_open(kSourceNamespace, NVS_READWRITE, &h);
  if (err != ESP_OK) return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
  err = nvs_erase_key(h, key.c_str());
  if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  return err;
}

esp_err_t CredentialStore::resolveSourceCredential(const std::string& ref, std::string& value) {
  value.clear();
  const std::string key = sourceKey(ref);
  if (key.empty()) return ESP_ERR_INVALID_ARG;
  if (ensureNvs() != ESP_OK) return ESP_FAIL;
  nvs_handle_t h;
  esp_err_t err = nvs_open(kSourceNamespace, NVS_READONLY, &h);
  if (err != ESP_OK) return err;
  size_t length = 0;
  err = nvs_get_str(h, key.c_str(), nullptr, &length);
  std::string temporary;
  if (err == ESP_OK && (length <= 1 || length > kSourceValueMaxBytes + 1)) {
    err = ESP_ERR_INVALID_SIZE;
  }
  if (err == ESP_OK) {
    temporary.resize(length - 1);
    size_t read_length = length;
    err = nvs_get_str(h, key.c_str(), temporary.data(), &read_length);
  }
  nvs_close(h);
  if (err != ESP_OK || !validSourceValue(temporary)) {
    secureErase(temporary);
    return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
  }
  value = std::move(temporary);
  return ESP_OK;
}

esp_err_t CredentialStore::listSourceCredentials(std::vector<SourceCredentialMetadata>& out) {
  out.clear();
  if (ensureNvs() != ESP_OK) return ESP_FAIL;
  nvs_iterator_t iterator = nullptr;
  esp_err_t err = nvs_entry_find("nvs", kSourceNamespace, NVS_TYPE_STR, &iterator);
  if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
  while (err == ESP_OK) {
    nvs_entry_info_t info;
    err = nvs_entry_info(iterator, &info);
    if (err != ESP_OK) break;
    const std::string key(info.key);
    if (validSourceKey(key)) out.push_back({key, sourceRef(key)});
    err = nvs_entry_next(&iterator);
  }
  nvs_release_iterator(iterator);
  if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
  if (err != ESP_OK) out.clear();
  return err;
}

esp_err_t CredentialStore::clearSourceCredentials() {
  return clearNamespace(kSourceNamespace);
}

void CredentialStore::secureErase(std::string& value) {
  volatile char* data = value.empty() ? nullptr : value.data();
  for (size_t i = 0; i < value.size(); ++i) data[i] = 0;
  value.clear();
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
