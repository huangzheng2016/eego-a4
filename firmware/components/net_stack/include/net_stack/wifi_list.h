// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// WiFi 网络列表的纯逻辑（无 ESP-IDF 依赖，主机测试覆盖）：
//   * 上限/字段校验（SSID 1..32B；密码空=开放网络，或 8..63B）；
//   * 编译期默认网络（local_secrets.h EEGO_LOCAL_WIFI_LIST）的 constexpr 校验与
//     确保语义（每次启动：默认网络始终存在、密码以编译期为准、排在最前；其余已存
//     网络保持相对顺序跟在后面；replace=true 只保留默认网络）；
//   * 断开原因码 → 名称/中文说明、认证方式名（诊断用）；
//   * 每个已存网络的最近尝试/结果/原因 + 最近一次扫描的离线诊断记录；
//   * 管理端整表编辑（顺序即优先级；"********"/缺省密码 = 保留原密码）；
//   * 连接尝试计划（扫描可见的已知网络按列表顺序优先，不可见的随后各试一次）。
// 列表顺序就是连接优先级：索引 0 最先尝试。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "net_stack/credentials.h"

namespace eego::net {

constexpr size_t kMaxWifiNetworks = 8;  // NVS "wifi" 命名空间布局上限
constexpr size_t kWifiSsidMaxBytes = 32;
constexpr size_t kWifiPasswordMinBytes = 8;
constexpr size_t kWifiPasswordMaxBytes = 63;
// 管理端回显/提交的密码占位：表示"保留已保存的密码"。
constexpr const char* kWifiPasswordKeep = "********";

bool validWifiSsid(const std::string& ssid);
bool validWifiPassword(const std::string& password);  // 空 = 开放网络

// ---- 编译期默认网络 ------------------------------------------------------------

struct WifiDefault {
  const char* ssid;
  const char* password;
};

constexpr size_t wifiCstrLen(const char* s) {
  size_t n = 0;
  while (s != nullptr && s[n] != '\0') n++;
  return n;
}

constexpr bool wifiCstrEq(const char* a, const char* b) {
  size_t i = 0;
  while (a[i] != '\0' && a[i] == b[i]) i++;
  return a[i] == b[i];
}

// static_assert 用：条目数 1..kMaxWifiNetworks，SSID 1..32B 且不重复，
// 密码为空或 8..63B。
template <size_t N>
constexpr bool validWifiDefaults(const WifiDefault (&list)[N]) {
  if (N == 0 || N > kMaxWifiNetworks) return false;
  for (size_t i = 0; i < N; i++) {
    if (list[i].ssid == nullptr || list[i].password == nullptr) return false;
    const size_t ssid_len = wifiCstrLen(list[i].ssid);
    const size_t pass_len = wifiCstrLen(list[i].password);
    if (ssid_len == 0 || ssid_len > kWifiSsidMaxBytes) return false;
    if (pass_len != 0 && (pass_len < kWifiPasswordMinBytes || pass_len > kWifiPasswordMaxBytes)) {
      return false;
    }
    for (size_t j = 0; j < i; j++) {
      if (wifiCstrEq(list[i].ssid, list[j].ssid)) return false;
    }
  }
  return true;
}

// 确保语义（每次启动调用）：defaults（跳过非法/重复条目，至多 kMaxWifiNetworks）
// 按给定顺序排在最前，密码一律取编译期值（与已存不同则更新）；stored 中其余网络
// 保持相对顺序跟在后面，超出上限时丢弃末尾（优先级最低）的非默认网络。
// replace=true：结果只含 defaults。结果与 stored 不同时写回 stored 并返回 true；
// 相同或无有效默认值时 stored 不变并返回 false。
bool seedWifiDefaults(std::vector<WifiAccount>& stored, const WifiDefault* defaults,
                      size_t count, bool replace);

// ---- 诊断：原因码 / 认证方式 ---------------------------------------------------

// wifi_err_reason_t 数值（esp_wifi_types_generic.h，ESP-IDF v5.5）→ 稳定英文名
// （如 "NO_AP_FOUND"；未知 → "UNKNOWN"；0 → "NONE"）。
const char* wifiReasonName(uint8_t reason);
// 面向用户的中文说明（含常见原因与建议）。0 = 无原因码（通常是超时）。
const char* wifiReasonText(uint8_t reason);
// 设备屏幕用的短说明（≤ 8 个汉字 / 16 个英文字符）。
const char* wifiReasonShort(uint8_t reason, bool english);
// wifi_auth_mode_t 数值 → 名称（"OPEN"/"WPA2_PSK"/…；未知 → "OTHER"）。
const char* wifiAuthModeName(int authmode);

// ---- 诊断：离线记录（WifiManager 持锁维护；纯逻辑便于主机测试）------------------

enum class WifiAttemptResult : uint8_t { None = 0, Connecting, Connected, Failed, Timeout };
const char* wifiAttemptResultName(WifiAttemptResult r);

struct WifiNetworkDiag {
  std::string ssid;
  int64_t last_attempt_ms = -1;  // 自启动 ms（-1 = 本次运行未尝试）
  int64_t last_result_ms = -1;
  WifiAttemptResult result = WifiAttemptResult::None;
  uint8_t last_reason = 0;       // 最近一次失败的断开原因码（0 = 无/超时）
  uint8_t first_reason = 0;      // 本次开机第一次失败的原因码（区分最初故障与后续连锁）
  int64_t first_failure_ms = -1;
  uint32_t attempts = 0;
  uint32_t failures = 0;
  bool seen = false;             // 最近一次扫描是否看到
  int8_t rssi = 0;               // seen 时最强信号
  uint8_t channel = 0;
  int authmode = -1;
};

struct WifiScanEntry {
  std::string ssid;
  int8_t rssi = 0;
  uint8_t channel = 0;
  int authmode = 0;  // wifi_auth_mode_t 数值
};

struct WifiDiagSnapshot {
  std::vector<WifiNetworkDiag> networks;  // 已存网络（列表顺序）
  std::vector<WifiScanEntry> scan;        // 最近一次扫描（同名合并保留最强，按信号降序）
  int64_t scan_ms = -1;                   // 最近一次扫描时间（-1 = 从未）
  bool scan_ok = false;
  uint32_t failed_passes = 0;             // 连续失败的整表遍历轮数（连上清零）
};

class WifiDiagLog {
 public:
  // 同步已存网络列表（顺序 = 优先级）；保留同名网络的既有记录。
  void syncSaved(const std::vector<std::string>& ssids);
  // 记录一次扫描（ok=false 表示扫描失败；entries 被合并/排序）。
  void recordScan(bool ok, std::vector<WifiScanEntry> entries, int64_t now_ms);
  void recordAttempt(const std::string& ssid, int64_t now_ms);
  void recordResult(const std::string& ssid, WifiAttemptResult result, uint8_t reason,
                    int64_t now_ms);
  const WifiDiagSnapshot& snapshot() const { return snap_; }

 private:
  WifiNetworkDiag* find(const std::string& ssid);
  void applySeen(WifiNetworkDiag& n) const;
  WifiDiagSnapshot snap_;
};

// ---- AP 兜底触发策略（纯逻辑）------------------------------------------------------
//
// 单射频：AP 打开后 STA 与 AP 必须分时共用信道，热点会拖累 STA 重连。所以不在第一轮
// 失败就开热点：连续 kApFallbackFailedPasses 轮整表遍历失败，或已存网络持续
// kApFallbackInvisibleMs 一个都扫不到时才到期；没有任何已存网络时立即到期。

constexpr uint32_t kApFallbackFailedPasses = 3;
constexpr int64_t kApFallbackInvisibleMs = 2 * 60 * 1000;

class ApFallbackPolicy {
 public:
  void onConnected() {
    failed_passes_ = 0;
    invisible_since_ms_ = -1;
  }
  // 一轮整表遍历失败；saved_visible = 本轮扫描看到了至少一个已存网络。
  void onPassFailed(int64_t now, bool saved_visible) {
    failed_passes_++;
    if (saved_visible) {
      invisible_since_ms_ = -1;
    } else if (invisible_since_ms_ < 0) {
      invisible_since_ms_ = now;
    }
  }
  bool due(int64_t now, size_t saved_networks) const {
    if (saved_networks == 0) return true;
    if (failed_passes_ >= kApFallbackFailedPasses) return true;
    return invisible_since_ms_ >= 0 && now - invisible_since_ms_ >= kApFallbackInvisibleMs;
  }
  uint32_t failedPasses() const { return failed_passes_; }

 private:
  uint32_t failed_passes_ = 0;
  int64_t invisible_since_ms_ = -1;
};

// ---- 管理端整表编辑 ------------------------------------------------------------

struct WifiListEntry {
  std::string ssid;
  std::string password;       // keep_password=false 时生效（空 = 开放网络）
  bool keep_password = false;  // true：沿用 previous_ssid（缺省为 ssid）的已存密码
  std::string previous_ssid;   // 改名时指向原 SSID，用于保留密码
};

struct WifiListError {
  std::string field;  // 例如 "networks[2].password"
  std::string message;
};

// 以 desired 的顺序生成新的完整列表（顺序 = 优先级）。失败返回 false 且 out 不变。
bool applyWifiListEdit(const std::vector<WifiAccount>& current,
                       const std::vector<WifiListEntry>& desired, std::vector<WifiAccount>& out,
                       WifiListError& error);

// ---- 连接尝试计划 --------------------------------------------------------------

struct WifiScanHit {
  std::string ssid;
  int8_t rssi = 0;
};

struct WifiAttempt {
  size_t index = 0;  // accounts 下标
  uint32_t timeout_ms = 0;
};

constexpr uint32_t kWifiVisibleFirstTimeoutMs = 30000;  // 可见网络首试（弱信号容忍）
constexpr uint32_t kWifiVisibleRetryTimeoutMs = 15000;  // 可见网络重试
constexpr uint32_t kWifiUnseenTimeoutMs = 12000;        // 扫描未见（隐藏 SSID/漏检）

// scan == nullptr 或为空（扫描失败/零结果）时按列表顺序每个网络两次尝试；否则
// 可见的已知网络按列表顺序各两次，随后未见网络按列表顺序各一次（短超时）。
// SSID 为空的条目被跳过。
std::vector<WifiAttempt> planWifiAttempts(const std::vector<WifiAccount>& accounts,
                                          const std::vector<WifiScanHit>* scan);

// ---- 重连调度（net_task 驱动；纯逻辑便于主机测试）-------------------------------
//
// 断线后立即重试一轮（遍历整表）；失败后按 start_ms 起翻倍、封顶 max_ms 的退避
// 继续，永不放弃。连续离线（Disconnected/Failed/Offline）满 kWifiDriverRestartMs
// 时请求一次 esp_wifi_stop/start 驱动重启（之后每满同样时长再一次）。连上即复位。

constexpr uint32_t kWifiBackoffStartMs = 10000;
constexpr uint32_t kWifiBackoffMaxMs = 60000;
constexpr int64_t kWifiDriverRestartMs = 5 * 60 * 1000;

class WifiReconnectScheduler {
 public:
  explicit WifiReconnectScheduler(uint32_t start_ms = kWifiBackoffStartMs,
                                  uint32_t max_ms = kWifiBackoffMaxMs)
      : start_ms_(start_ms), max_ms_(max_ms < start_ms ? start_ms : max_ms),
        backoff_ms_(start_ms) {}

  void onConnected(int64_t now);
  // 链路不在（含启动后尚未连上）：首次调用记录离线起点。
  void onOffline(int64_t now);
  bool due(int64_t now) const { return now >= next_retry_ms_; }
  // 一轮 connectMulti 结束仍未连上：排下一次并加大退避。
  void onAttemptFailed(int64_t now);
  // 立即重试（reconnectNow/配网结束）：退避复位，不清离线起点。
  void kick(int64_t now);
  // 暂缓到 when（如配网占用期间）。
  void deferUntil(int64_t when) { next_retry_ms_ = when; }
  bool driverRestartDue(int64_t now) const;
  void onDriverRestarted(int64_t now) { last_restart_ms_ = now; }

  uint32_t backoffMs() const { return backoff_ms_; }
  int64_t nextRetryMs() const { return next_retry_ms_; }
  int64_t offlineSinceMs() const { return offline_since_ms_; }  // -1 = 在线
  uint32_t attempts() const { return attempts_; }               // 累计重连轮数

 private:
  uint32_t start_ms_;
  uint32_t max_ms_;
  uint32_t backoff_ms_;
  int64_t next_retry_ms_ = 0;
  int64_t offline_since_ms_ = -1;
  int64_t last_restart_ms_ = -1;
  uint32_t attempts_ = 0;
};

}  // namespace eego::net
