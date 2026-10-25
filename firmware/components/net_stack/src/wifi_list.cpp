// SPDX-License-Identifier: GPL-3.0-or-later
// WiFi 网络列表纯逻辑（见 wifi_list.h）。不得输出任何密码。
#include "net_stack/wifi_list.h"

#include <cstring>
#include <utility>

namespace eego::net {

bool validWifiSsid(const std::string& ssid) {
  return !ssid.empty() && ssid.size() <= kWifiSsidMaxBytes &&
         ssid.find('\0') == std::string::npos;
}

bool validWifiPassword(const std::string& password) {
  if (password.empty()) return true;
  return password.size() >= kWifiPasswordMinBytes && password.size() <= kWifiPasswordMaxBytes &&
         password.find('\0') == std::string::npos;
}

bool seedWifiDefaults(std::vector<WifiAccount>& stored, const WifiDefault* defaults,
                      size_t count, bool replace) {
  std::vector<WifiAccount> result;
  for (size_t i = 0; defaults != nullptr && i < count && result.size() < kMaxWifiNetworks; i++) {
    if (defaults[i].ssid == nullptr) continue;
    WifiAccount account{defaults[i].ssid, defaults[i].password ? defaults[i].password : ""};
    bool duplicate = false;
    for (const auto& existing : result) duplicate = duplicate || existing.ssid == account.ssid;
    if (duplicate || !validWifiSsid(account.ssid) || !validWifiPassword(account.password)) {
      CredentialStore::secureErase(account.password);
      continue;
    }
    result.push_back(std::move(account));
  }
  if (result.empty()) return false;  // 无有效默认值：不动 NVS
  const size_t default_count = result.size();
  if (!replace) {
    for (const auto& account : stored) {
      if (result.size() >= kMaxWifiNetworks) break;  // 末尾（最低优先级）的非默认网络被丢弃
      bool is_default = false;
      for (size_t i = 0; i < default_count; i++) is_default = is_default || result[i].ssid == account.ssid;
      if (!is_default) result.push_back(account);
    }
  }
  bool same = result.size() == stored.size();
  for (size_t i = 0; same && i < result.size(); i++) {
    same = result[i].ssid == stored[i].ssid && result[i].password == stored[i].password;
  }
  if (same) {
    for (auto& account : result) CredentialStore::secureErase(account.password);
    return false;
  }
  for (auto& account : stored) CredentialStore::secureErase(account.password);
  stored = std::move(result);
  return true;
}

// ---- 诊断：原因码 / 认证方式 ----------------------------------------------------
// 数值取自 ESP-IDF v5.5 esp_wifi_types_generic.h wifi_err_reason_t（纯数值，主机可测）。

const char* wifiReasonName(uint8_t reason) {
  switch (reason) {
    case 0: return "NONE";
    case 1: return "UNSPECIFIED";
    case 2: return "AUTH_EXPIRE";
    case 3: return "AUTH_LEAVE";
    case 4: return "ASSOC_EXPIRE";
    case 5: return "ASSOC_TOOMANY";
    case 6: return "NOT_AUTHED";
    case 7: return "NOT_ASSOCED";
    case 8: return "ASSOC_LEAVE";
    case 9: return "ASSOC_NOT_AUTHED";
    case 13: return "IE_INVALID";
    case 14: return "MIC_FAILURE";
    case 15: return "4WAY_HANDSHAKE_TIMEOUT";
    case 16: return "GROUP_KEY_UPDATE_TIMEOUT";
    case 17: return "IE_IN_4WAY_DIFFERS";
    case 18: return "GROUP_CIPHER_INVALID";
    case 19: return "PAIRWISE_CIPHER_INVALID";
    case 20: return "AKMP_INVALID";
    case 23: return "802_1X_AUTH_FAILED";
    case 24: return "CIPHER_SUITE_REJECTED";
    case 200: return "BEACON_TIMEOUT";
    case 201: return "NO_AP_FOUND";
    case 202: return "AUTH_FAIL";
    case 203: return "ASSOC_FAIL";
    case 204: return "HANDSHAKE_TIMEOUT";
    case 205: return "CONNECTION_FAIL";
    case 206: return "AP_TSF_RESET";
    case 207: return "ROAMING";
    case 208: return "ASSOC_COMEBACK_TIME_TOO_LONG";
    case 209: return "SA_QUERY_TIMEOUT";
    case 210: return "NO_AP_FOUND_W_COMPATIBLE_SECURITY";
    case 211: return "NO_AP_FOUND_IN_AUTHMODE_THRESHOLD";
    case 212: return "NO_AP_FOUND_IN_RSSI_THRESHOLD";
    default: return "UNKNOWN";
  }
}

const char* wifiReasonText(uint8_t reason) {
  switch (reason) {
    case 0: return "连接超时（未收到路由器响应）";
    case 2: return "认证超时：路由器未响应（信号弱或路由器繁忙）";
    case 3: return "路由器主动取消认证";
    case 4: return "关联超时：路由器未响应";
    case 5: return "路由器已连接设备过多";
    case 6:
    case 7:
    case 9: return "路由器拒绝：未完成认证/关联";
    case 8: return "本机主动断开（重试或切换网络时正常出现）";
    case 14: return "密码错误（MIC 校验失败）";
    case 15: return "四次握手超时：多为密码错误，也可能是信号弱";
    case 16: return "组密钥更新超时（信号弱）";
    case 18:
    case 19:
    case 20:
    case 24: return "加密方式不兼容（请用 WPA2-PSK/AES）";
    case 23: return "802.1X 企业认证失败（不支持企业网络）";
    case 200: return "信号丢失（信标超时）";
    case 201: return "找不到该网络：SSID 不可见、仅 5GHz 或超出范围";
    case 202: return "认证失败：密码错误或路由器拒绝";
    case 203: return "关联失败：路由器拒绝（检查 MAC 过滤/连接数上限）";
    case 204: return "握手超时：多为密码错误";
    case 205: return "连接失败";
    case 210: return "找到同名网络但加密方式不兼容（如仅企业认证）";
    case 211: return "找到同名网络但加密方式低于要求（开放/WEP 网络却填了密码？）";
    case 212: return "找到同名网络但信号过弱";
    default: return "连接失败（其他原因）";
  }
}

const char* wifiReasonShort(uint8_t reason, bool english) {
  switch (reason) {
    case 0: return english ? "TIMEOUT" : "连接超时";
    case 2:
    case 4: return english ? "NO RESPONSE" : "路由器无响应";
    case 5: return english ? "AP FULL" : "路由器已满";
    case 8: return english ? "LEFT" : "主动断开";
    case 14:
    case 15:
    case 204: return english ? "WRONG PASSWORD?" : "密码错误？";
    case 202: return english ? "AUTH FAILED" : "认证失败";
    case 203: return english ? "REJECTED" : "关联被拒";
    case 200: return english ? "SIGNAL LOST" : "信号丢失";
    case 201: return english ? "NOT FOUND" : "未找到网络";
    case 18:
    case 19:
    case 20:
    case 24:
    case 210:
    case 211: return english ? "SECURITY MISMATCH" : "加密不兼容";
    case 212: return english ? "WEAK SIGNAL" : "信号过弱";
    default: return english ? "FAILED" : "连接失败";
  }
}

// wifi_auth_mode_t（v5.5）：0 OPEN … 13 DPP；更新的枚举值统一 OTHER。
const char* wifiAuthModeName(int authmode) {
  switch (authmode) {
    case 0: return "OPEN";
    case 1: return "WEP";
    case 2: return "WPA_PSK";
    case 3: return "WPA2_PSK";
    case 4: return "WPA_WPA2_PSK";
    case 5: return "ENTERPRISE";
    case 6: return "WPA3_PSK";
    case 7: return "WPA2_WPA3_PSK";
    case 8: return "WAPI_PSK";
    case 9: return "OWE";
    case 10: return "WPA3_ENT_192";
    case 11: return "WPA3_EXT_PSK";
    case 12: return "WPA3_EXT_PSK_MIXED";
    case 13: return "DPP";
    default: return "OTHER";
  }
}

// ---- 诊断：离线记录 ----------------------------------------------------------------

const char* wifiAttemptResultName(WifiAttemptResult r) {
  switch (r) {
    case WifiAttemptResult::None: return "none";
    case WifiAttemptResult::Connecting: return "connecting";
    case WifiAttemptResult::Connected: return "connected";
    case WifiAttemptResult::Failed: return "failed";
    case WifiAttemptResult::Timeout: return "timeout";
  }
  return "none";
}

WifiNetworkDiag* WifiDiagLog::find(const std::string& ssid) {
  for (auto& n : snap_.networks) {
    if (n.ssid == ssid) return &n;
  }
  return nullptr;
}

void WifiDiagLog::applySeen(WifiNetworkDiag& n) const {
  n.seen = false;
  n.rssi = 0;
  n.channel = 0;
  n.authmode = -1;
  for (const auto& e : snap_.scan) {
    if (e.ssid != n.ssid) continue;
    n.seen = true;  // scan 已按信号降序：首个即最强
    n.rssi = e.rssi;
    n.channel = e.channel;
    n.authmode = e.authmode;
    return;
  }
}

void WifiDiagLog::syncSaved(const std::vector<std::string>& ssids) {
  std::vector<WifiNetworkDiag> next;
  next.reserve(ssids.size());
  for (const auto& ssid : ssids) {
    const WifiNetworkDiag* old = find(ssid);
    WifiNetworkDiag n = old != nullptr ? *old : WifiNetworkDiag{};
    n.ssid = ssid;
    applySeen(n);
    next.push_back(std::move(n));
  }
  snap_.networks = std::move(next);
}

void WifiDiagLog::recordScan(bool ok, std::vector<WifiScanEntry> entries, int64_t now_ms) {
  snap_.scan_ms = now_ms;
  snap_.scan_ok = ok;
  std::vector<WifiScanEntry> merged;
  for (auto& e : entries) {
    if (e.ssid.empty()) continue;  // 隐藏 SSID
    bool dup = false;
    for (auto& m : merged) {
      if (m.ssid != e.ssid) continue;
      dup = true;
      if (e.rssi > m.rssi) m = e;
      break;
    }
    if (!dup) merged.push_back(std::move(e));
  }
  // 插入排序（≤ 几十条）：信号降序，稳定。
  for (size_t i = 1; i < merged.size(); i++) {
    for (size_t j = i; j > 0 && merged[j].rssi > merged[j - 1].rssi; j--) {
      std::swap(merged[j], merged[j - 1]);
    }
  }
  constexpr size_t kMaxScanEntries = 32;
  if (merged.size() > kMaxScanEntries) merged.resize(kMaxScanEntries);
  snap_.scan = std::move(merged);
  for (auto& n : snap_.networks) applySeen(n);
}

void WifiDiagLog::recordAttempt(const std::string& ssid, int64_t now_ms) {
  WifiNetworkDiag* n = find(ssid);
  if (n == nullptr) return;
  n->attempts++;
  n->last_attempt_ms = now_ms;
  n->result = WifiAttemptResult::Connecting;
}

void WifiDiagLog::recordResult(const std::string& ssid, WifiAttemptResult result,
                               uint8_t reason, int64_t now_ms) {
  WifiNetworkDiag* n = find(ssid);
  if (n == nullptr) return;
  n->result = result;
  n->last_result_ms = now_ms;
  if (result == WifiAttemptResult::Failed || result == WifiAttemptResult::Timeout) {
    if (n->failures == 0) {
      n->first_reason = reason;
      n->first_failure_ms = now_ms;
    }
    n->failures++;
    n->last_reason = reason;
  }
}

bool applyWifiListEdit(const std::vector<WifiAccount>& current,
                       const std::vector<WifiListEntry>& desired, std::vector<WifiAccount>& out,
                       WifiListError& error) {
  if (desired.size() > kMaxWifiNetworks) {
    error = {"networks", "too many networks (max " + std::to_string(kMaxWifiNetworks) + ")"};
    return false;
  }
  std::vector<WifiAccount> result;
  result.reserve(desired.size());
  auto fail = [&](size_t i, const char* field, const char* message) {
    error = {"networks[" + std::to_string(i) + "]." + field, message};
    for (auto& account : result) CredentialStore::secureErase(account.password);
    return false;
  };
  for (size_t i = 0; i < desired.size(); i++) {
    const WifiListEntry& entry = desired[i];
    if (!validWifiSsid(entry.ssid)) return fail(i, "ssid", "must be 1..32 bytes");
    for (const auto& prior : result) {
      if (prior.ssid == entry.ssid) return fail(i, "ssid", "duplicate network");
    }
    WifiAccount account;
    account.ssid = entry.ssid;
    if (entry.keep_password) {
      const std::string& source = entry.previous_ssid.empty() ? entry.ssid : entry.previous_ssid;
      const WifiAccount* saved = nullptr;
      for (const auto& candidate : current) {
        if (candidate.ssid == source) saved = &candidate;
      }
      if (saved == nullptr) return fail(i, "password", "required for a new network");
      account.password = saved->password;
    } else {
      if (!validWifiPassword(entry.password)) {
        return fail(i, "password", "must be empty (open) or 8..63 bytes");
      }
      account.password = entry.password;
    }
    result.push_back(std::move(account));
  }
  for (auto& account : out) CredentialStore::secureErase(account.password);
  out = std::move(result);
  return true;
}

std::vector<WifiAttempt> planWifiAttempts(const std::vector<WifiAccount>& accounts,
                                          const std::vector<WifiScanHit>* scan) {
  std::vector<WifiAttempt> plan;
  const bool have_scan = scan != nullptr && !scan->empty();
  auto visible = [&](const std::string& ssid) {
    if (!have_scan) return true;
    for (const auto& hit : *scan) {
      if (hit.ssid == ssid) return true;
    }
    return false;
  };
  for (size_t i = 0; i < accounts.size(); i++) {
    if (accounts[i].ssid.empty() || !visible(accounts[i].ssid)) continue;
    plan.push_back({i, kWifiVisibleFirstTimeoutMs});
    plan.push_back({i, kWifiVisibleRetryTimeoutMs});
  }
  if (have_scan) {
    for (size_t i = 0; i < accounts.size(); i++) {
      if (accounts[i].ssid.empty() || visible(accounts[i].ssid)) continue;
      plan.push_back({i, kWifiUnseenTimeoutMs});
    }
  }
  return plan;
}

void WifiReconnectScheduler::onConnected(int64_t now) {
  (void)now;
  backoff_ms_ = start_ms_;
  next_retry_ms_ = 0;
  offline_since_ms_ = -1;
  last_restart_ms_ = -1;
}

void WifiReconnectScheduler::onOffline(int64_t now) {
  if (offline_since_ms_ < 0) offline_since_ms_ = now;
}

void WifiReconnectScheduler::onAttemptFailed(int64_t now) {
  attempts_++;
  next_retry_ms_ = now + backoff_ms_;
  const uint64_t doubled = static_cast<uint64_t>(backoff_ms_) * 2;
  backoff_ms_ = doubled > max_ms_ ? max_ms_ : static_cast<uint32_t>(doubled);
}

void WifiReconnectScheduler::kick(int64_t now) {
  backoff_ms_ = start_ms_;
  next_retry_ms_ = now;
}

bool WifiReconnectScheduler::driverRestartDue(int64_t now) const {
  if (offline_since_ms_ < 0) return false;
  const int64_t since = last_restart_ms_ >= 0 ? last_restart_ms_ : offline_since_ms_;
  return now - since >= kWifiDriverRestartMs;
}

// ---- 信号质量 / 链路统计 / 漫游 ------------------------------------------------

WifiSignal wifiSignalQuality(int rssi) {
  if (rssi >= 0) return WifiSignal::None;
  if (rssi >= kWifiSignalExcellentDbm) return WifiSignal::Excellent;
  if (rssi >= kWifiSignalGoodDbm) return WifiSignal::Good;
  if (rssi >= kWifiSignalFairDbm) return WifiSignal::Fair;
  return WifiSignal::Weak;
}

const char* wifiSignalName(WifiSignal s) {
  switch (s) {
    case WifiSignal::Excellent: return "excellent";
    case WifiSignal::Good: return "good";
    case WifiSignal::Fair: return "fair";
    case WifiSignal::Weak: return "weak";
    case WifiSignal::None: break;
  }
  return "none";
}

const char* wifiSignalLabel(WifiSignal s, bool english) {
  switch (s) {
    case WifiSignal::Excellent: return english ? "EXCELLENT" : "很好";
    case WifiSignal::Good: return english ? "GOOD" : "良好";
    case WifiSignal::Fair: return english ? "FAIR" : "一般";
    case WifiSignal::Weak: return english ? "WEAK" : "很弱";
    case WifiSignal::None: break;
  }
  return "—";
}

int wifiSignalBars(WifiSignal s) {
  switch (s) {
    case WifiSignal::Excellent: return 4;
    case WifiSignal::Good: return 3;
    case WifiSignal::Fair: return 2;
    case WifiSignal::Weak: return 1;
    case WifiSignal::None: break;
  }
  return 0;
}

const char kWifiWeakSignalHint[] =
    "信号很弱：建议把设备移近路由器、固定 2.4GHz 信道（1/6/11）、20MHz 频宽";

void WifiLinkStats::add(int64_t now, int rssi) {
  last_ms_ = now;
  if (rssi >= 0 || rssi < -128) return;
  ring_[head_] = Sample{now, static_cast<int8_t>(rssi)};
  head_ = (head_ + 1) % kCap;
  if (count_ < kCap) count_++;
}

WifiLinkSummary WifiLinkStats::summary(int64_t now) const {
  WifiLinkSummary out;
  int sum = 0;
  int64_t newest = -1;
  for (size_t i = 0; i < count_; i++) {
    const Sample& s = ring_[(head_ + kCap - 1 - i) % kCap];
    if (now - s.at > kWifiLinkWindowMs) break;  // 由新到旧，越界即止
    if (out.samples == 0 || s.rssi < out.min) out.min = s.rssi;
    if (s.at > newest) {
      newest = s.at;
      out.last = s.rssi;
    }
    sum += s.rssi;
    out.samples++;
  }
  if (out.samples > 0) {
    // 向零取整的负数平均会偏乐观：按四舍五入。
    const int n = static_cast<int>(out.samples);
    out.avg = static_cast<int8_t>((sum - n / 2) / n);
  }
  return out;
}

bool WifiRoamPolicy::update(int64_t now, int rssi) {
  if (rssi >= 0 || rssi >= kWifiRoamWeakDbm) {
    weak_since_ms_ = -1;
    return false;
  }
  if (weak_since_ms_ < 0) weak_since_ms_ = now;
  if (now - weak_since_ms_ < kWifiRoamWeakMs) return false;
  return last_scan_ms_ < 0 || now - last_scan_ms_ >= kWifiRoamIntervalMs;
}

int pickRoamTarget(const uint8_t current_bssid[6], int current_rssi,
                   const std::vector<WifiBssCandidate>& candidates) {
  int best = -1;
  for (size_t i = 0; i < candidates.size(); i++) {
    const WifiBssCandidate& c = candidates[i];
    if (current_bssid != nullptr && std::memcmp(c.bssid, current_bssid, 6) == 0) continue;
    if (c.channel == 0 || c.rssi >= 0) continue;
    if (static_cast<int>(c.rssi) < current_rssi + kWifiRoamMinGainDb) continue;
    if (best < 0 || c.rssi > candidates[static_cast<size_t>(best)].rssi) best = static_cast<int>(i);
  }
  return best;
}

}  // namespace eego::net
