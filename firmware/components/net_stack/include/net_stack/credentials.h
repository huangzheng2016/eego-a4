// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// NVS 凭据存储 —— WiFi 账号与源认证 token 的唯一读写接口（M13 定稿）。
//
// 密钥边界（M13 §4）：
//   * WiFi 密码 / 管理密码 hash / ingest key / 源 auth token 只存 NVS
//     （"wifi" / "sec" 命名空间，自管读写，绕开 sdkconfig 禁用 esp_wifi NVS 的限制）；
//   * SD config.json 只放非机密配置与 "*_ref" 引用（admin 组件契约，M17）；
//   * 本组件任何日志不得输出凭据值（M13：secrets 仅允许前 4 位掩码，这里一律不打印）。
//
// 数据源公开引用固定为 "nvs:source/<key>"，只允许 source 命名空间；
// 内部管理认证仍可解析 "nvs:<key>"（默认 sec）或 "nvs:<ns>/<key>"，
// 但不得把该内部能力用于外部数据源配置。

#include <string>
#include <vector>

#include "esp_err.h"

namespace eego::net {

struct WifiAccount {
  std::string ssid;
  std::string password;  // 仅内存使用；禁止写入日志/SD/告警字段
};

struct SourceCredentialMetadata {
  std::string key;
  std::string ref;
};

class CredentialStore {
 public:
  static constexpr const char* kSourceNamespace = "source";
  static constexpr size_t kSourceKeyMaxBytes = 15;
  static constexpr size_t kSourceValueMaxBytes = 1024;

  // 确保 nvs_flash 已初始化（幂等）。返回 ESP_OK 或初始化错误；
  // 无空闲页/版本变化时执行一次 erase 重建（IDF 标准恢复路径）。
  static esp_err_t ensureNvs();

  // ---- WiFi 账号（NVS 命名空间 "wifi"）----
  // 布局：count(u8) + ssid<i>(str) + pass<i>(str)，i ∈ [0, count)。
  // load 返回账号数；NVS 未初始化或为空返回 0（不视为错误）。
  static size_t loadWifiAccounts(std::vector<WifiAccount>& out);
  static esp_err_t saveWifiAccounts(const std::vector<WifiAccount>& in);
  static esp_err_t clearWifiAccounts();

  // 仅供管理认证等内部调用方使用；数据源不得用此接口解析外部配置中的 ref。
  static std::string resolveToken(const std::string& ref);
  static esp_err_t saveToken(const std::string& ns, const std::string& key,
                             const std::string& value);
  static esp_err_t clearNamespace(const std::string& ns);

  // ---- 数据源凭据（固定 NVS 命名空间 "source"）----
  static bool validSourceKey(const std::string& key);
  static bool validSourceKeyForWrite(const std::string& key);
  static bool validSourceValue(const std::string& value);
  static bool validSourceRef(const std::string& ref);
  static std::string sourceRef(const std::string& key);
  static std::string sourceKey(const std::string& ref);
  static esp_err_t saveSourceCredential(const std::string& key, const std::string& value);
  static esp_err_t deleteSourceCredential(const std::string& key);
  static esp_err_t resolveSourceCredential(const std::string& ref, std::string& value);
  static esp_err_t listSourceCredentials(std::vector<SourceCredentialMetadata>& out);
  static esp_err_t clearSourceCredentials();

  static bool validHeaderName(const std::string& name);
  static bool safeHeaderValue(const std::string& value);
  static bool sensitiveHeaderName(const std::string& name);
  static void secureErase(std::string& value);

  // 供管理端/日志使用的安全标识：仅说明 ref 指向的 命名空间/键，绝不返回值。
  static std::string describeRef(const std::string& ref);
};

}  // namespace eego::net
