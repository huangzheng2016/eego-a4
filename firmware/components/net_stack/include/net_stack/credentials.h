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
// 引用格式（config.json 中的 *_ref）：
//   * "nvs:<key>"            → 默认命名空间 "sec"，键 <key>
//   * "nvs:<ns>/<key>"       → 命名空间 <ns>，键 <key>（如 "nvs:wifi/0"）
//   非 "nvs:" 前缀、缺段或读取失败一律解析为空串（调用方按"无凭据"处理）。

#include <string>
#include <vector>

#include "esp_err.h"

namespace eego::net {

struct WifiAccount {
  std::string ssid;
  std::string password;  // 仅内存使用；禁止写入日志/SD/告警字段
};

class CredentialStore {
 public:
  // 确保 nvs_flash 已初始化（幂等）。返回 ESP_OK 或初始化错误；
  // 无空闲页/版本变化时执行一次 erase 重建（IDF 标准恢复路径）。
  static esp_err_t ensureNvs();

  // ---- WiFi 账号（NVS 命名空间 "wifi"）----
  // 布局：count(u8) + ssid<i>(str) + pass<i>(str)，i ∈ [0, count)。
  // load 返回账号数；NVS 未初始化或为空返回 0（不视为错误）。
  static size_t loadWifiAccounts(std::vector<WifiAccount>& out);
  static esp_err_t saveWifiAccounts(const std::vector<WifiAccount>& in);
  static esp_err_t clearWifiAccounts();

  // ---- 源认证 token / 任意机密（默认命名空间 "sec"）----
  // resolveToken 解析 "*_ref" 引用；读不到返回空串（不区分不存在与读取失败，
  // 避免把 NVS 状态细节泄漏给上层）。
  static std::string resolveToken(const std::string& ref);
  static esp_err_t saveToken(const std::string& ns, const std::string& key,
                             const std::string& value);
  static esp_err_t clearNamespace(const std::string& ns);

  // 供管理端/日志使用的安全标识：仅说明 ref 指向的 命名空间/键，绝不返回值。
  static std::string describeRef(const std::string& ref);
};

}  // namespace eego::net
