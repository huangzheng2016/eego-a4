// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "net_stack/credentials.h"

namespace eego::net {
constexpr size_t kMaxWifiNetworks = 8;
constexpr size_t kWifiSsidMaxBytes = 32;
constexpr size_t kWifiPasswordMinBytes = 8;
constexpr size_t kWifiPasswordMaxBytes = 63;
constexpr const char* kWifiPasswordKeep = "********";

bool validWifiSsid(const std::string& ssid);
bool validWifiPassword(const std::string& password);

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

bool seedWifiDefaults(std::vector<WifiAccount>& stored, const WifiDefault* defaults,
                      size_t count, bool replace);

const char* wifiReasonName(uint8_t reason);
const char* wifiReasonText(uint8_t reason);
const char* wifiReasonShort(uint8_t reason, bool english);
const char* wifiAuthModeName(int authmode);

enum class WifiAttemptResult : uint8_t { None = 0, Connecting, Connected, Failed, Timeout };
const char* wifiAttemptResultName(WifiAttemptResult r);

struct WifiNetworkDiag {
  std::string ssid;
  int64_t last_attempt_ms = -1;
  int64_t last_result_ms = -1;
  WifiAttemptResult result = WifiAttemptResult::None;
  uint8_t last_reason = 0;
  uint8_t first_reason = 0;
  int64_t first_failure_ms = -1;
  uint32_t attempts = 0;
  uint32_t failures = 0;
  bool seen = false;
  int8_t rssi = 0;
  uint8_t channel = 0;
  int authmode = -1;
};

struct WifiScanEntry {
  std::string ssid;
  int8_t rssi = 0;
  uint8_t channel = 0;
  int authmode = 0;
};

struct WifiDiagSnapshot {
  std::vector<WifiNetworkDiag> networks;
  std::vector<WifiScanEntry> scan;
  int64_t scan_ms = -1;
  bool scan_ok = false;
  uint32_t failed_passes = 0;
};

class WifiDiagLog {
 public:
  void syncSaved(const std::vector<std::string>& ssids);
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

//

constexpr uint32_t kApFallbackFailedPasses = 3;
constexpr int64_t kApFallbackInvisibleMs = 2 * 60 * 1000;

class ApFallbackPolicy {
 public:
  void onConnected() {
    failed_passes_ = 0;
    invisible_since_ms_ = -1;
  }
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

//

enum class WifiSignal : uint8_t { None = 0, Weak, Fair, Good, Excellent };

constexpr int kWifiSignalExcellentDbm = -60;
constexpr int kWifiSignalGoodDbm = -70;
constexpr int kWifiSignalFairDbm = -80;

WifiSignal wifiSignalQuality(int rssi);
const char* wifiSignalName(WifiSignal s);                 // "excellent"/"good"/"fair"/"weak"/"none"
const char* wifiSignalLabel(WifiSignal s, bool english);
int wifiSignalBars(WifiSignal s);                         // 0..4
extern const char kWifiWeakSignalHint[];

constexpr int64_t kWifiLinkSampleMs = 10000;
constexpr int64_t kWifiLinkWindowMs = 10 * 60 * 1000;

struct WifiLinkSummary {
  uint32_t samples = 0;
  int8_t min = 0;
  int8_t avg = 0;
  int8_t last = 0;
};

class WifiLinkStats {
 public:
  bool sampleDue(int64_t now) const { return last_ms_ < 0 || now - last_ms_ >= kWifiLinkSampleMs; }
  void add(int64_t now, int rssi);
  WifiLinkSummary summary(int64_t now) const;

 private:
  static constexpr size_t kCap = kWifiLinkWindowMs / kWifiLinkSampleMs + 4;
  struct Sample {
    int64_t at = 0;
    int8_t rssi = 0;
  };
  Sample ring_[kCap];
  size_t head_ = 0;
  size_t count_ = 0;
  int64_t last_ms_ = -1;
};

//

constexpr int kWifiRoamWeakDbm = -80;
constexpr int64_t kWifiRoamWeakMs = 60 * 1000;
constexpr int kWifiRoamMinGainDb = 8;
constexpr int64_t kWifiRoamIntervalMs = 5 * 60 * 1000;

struct WifiBssCandidate {
  uint8_t bssid[6] = {0, 0, 0, 0, 0, 0};
  uint8_t channel = 0;
  int8_t rssi = 0;
};

class WifiRoamPolicy {
 public:
  void onConnected() { weak_since_ms_ = -1; }
  bool update(int64_t now, int rssi);
  void onScanned(int64_t now) {
    last_scan_ms_ = now;
    weak_since_ms_ = -1;
  }
  int64_t weakSinceMs() const { return weak_since_ms_; }

 private:
  int64_t weak_since_ms_ = -1;
  int64_t last_scan_ms_ = -1;
};

int pickRoamTarget(const uint8_t current_bssid[6], int current_rssi,
                   const std::vector<WifiBssCandidate>& candidates);

constexpr int64_t kWifiAdminActiveMs = 30000;
constexpr bool wifiModemSleepWanted(bool mains, bool keep_awake, bool battery_interactive,
                                    bool admin_active = false) {
  return !mains && !keep_awake && !admin_active && battery_interactive;
}
constexpr bool wifiAdminActive(int64_t now_ms, int64_t last_admin_ms) {
  return last_admin_ms > 0 && now_ms - last_admin_ms < kWifiAdminActiveMs;
}

constexpr int64_t kWifiDhcpGraceMs = 30000;
constexpr int64_t kWifiDhcpSlowMs = 10000;
constexpr int64_t wifiConnectDeadlineMs(int64_t start_ms, uint32_t timeout_ms, int64_t assoc_ms) {
  const int64_t base = start_ms + static_cast<int64_t>(timeout_ms);
  if (assoc_ms < 0) return base;
  const int64_t grace = assoc_ms + kWifiDhcpGraceMs;
  return grace > base ? grace : base;
}

struct WifiListEntry {
  std::string ssid;
  std::string password;
  bool keep_password = false;
  std::string previous_ssid;
};

struct WifiListError {
  std::string field;
  std::string message;
};

bool applyWifiListEdit(const std::vector<WifiAccount>& current,
                       const std::vector<WifiListEntry>& desired, std::vector<WifiAccount>& out,
                       WifiListError& error);

struct WifiScanHit {
  std::string ssid;
  int8_t rssi = 0;
};

struct WifiAttempt {
  size_t index = 0;
  uint32_t timeout_ms = 0;
};

constexpr uint32_t kWifiVisibleFirstTimeoutMs = 30000;
constexpr uint32_t kWifiVisibleRetryTimeoutMs = 15000;
constexpr uint32_t kWifiUnseenTimeoutMs = 12000;

std::vector<WifiAttempt> planWifiAttempts(const std::vector<WifiAccount>& accounts,
                                          const std::vector<WifiScanHit>* scan);

//

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
  void onOffline(int64_t now);
  bool due(int64_t now) const { return now >= next_retry_ms_; }
  void onAttemptFailed(int64_t now);
  void kick(int64_t now);
  void deferUntil(int64_t when) { next_retry_ms_ = when; }
  bool driverRestartDue(int64_t now) const;
  void onDriverRestarted(int64_t now) { last_restart_ms_ = now; }

  uint32_t backoffMs() const { return backoff_ms_; }
  int64_t nextRetryMs() const { return next_retry_ms_; }
  int64_t offlineSinceMs() const { return offline_since_ms_; }
  uint32_t attempts() const { return attempts_; }

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
