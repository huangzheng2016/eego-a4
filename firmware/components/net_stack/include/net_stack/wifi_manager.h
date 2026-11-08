// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

//
//
//
//

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "freertos/semphr.h"

#include "net_stack/credentials.h"
#include "net_stack/wifi_list.h"

namespace eego::net {
enum class WifiState : uint8_t {
  Disconnected = 0,
  Connecting,
  Connected,
  Failed,
  OfflineMode
};

const char* wifiStateName(WifiState s);

struct ApHint {
  bool valid = false;
  std::string ssid;
  uint8_t bssid[6] = {0, 0, 0, 0, 0, 0};
  uint8_t channel = 0;
};

class WifiManager {
 public:
  static WifiManager& instance();

  esp_err_t init();

  WifiState connectMulti(const std::vector<WifiAccount>& accounts);

  WifiState connect(const WifiAccount& acc, uint32_t timeout_ms = 15000,
                    const ApHint* hint = nullptr);

  void setFastConnectHint(const ApHint& hint);
  ApHint lastConnectedAp() const;

  void disconnect();

  void powerOff();

  esp_err_t restartDriver();

  WifiState state() const { return state_; }
  bool isConnected() const { return state_ == WifiState::Connected; }
  int8_t rssi() const;
  std::string currentSsid() const { return current_ssid_; }
  std::string currentIp() const;
  uint8_t lastReason() const { return last_reason_; }
  uint8_t lastDisconnectReason() const { return last_disconnect_reason_; }
  uint32_t driverRestarts() const { return driver_restarts_; }

  void setPowerSave(bool enable);

  WifiDiagSnapshot diagnostics() const;
  bool scanNow();
  void syncSavedNetworks(const std::vector<WifiAccount>& accounts);
  std::string summary() const;
  bool fastHintPending() const { return fast_hint_.valid; }

  void applyRadioTuning();

  bool serviceLink(int64_t now_ms);
  bool roam(const std::vector<WifiAccount>& accounts);
  void deferRoamUntil(int64_t ms) { roam_defer_until_ms_.store(ms); }

  struct LinkInfo {
    WifiLinkSummary rssi;
    uint32_t beacon_timeouts = 0;
    uint32_t roams = 0;
    uint32_t roam_scans = 0;
    int32_t last_assoc_ms = -1;
    int32_t last_dhcp_ms = -1;
    int8_t tx_power_qdbm = 0;
  };
  LinkInfo linkInfo() const;

  bool apFallbackDue(int64_t now_ms) const;
  uint8_t bestSavedChannel() const;

 private:
  WifiManager();
  WifiState state_ = WifiState::Disconnected;
  bool initialized_ = false;
  bool started_ = false;
  volatile uint8_t last_disconnect_reason_ = 0;
  uint32_t driver_restarts_ = 0;
  bool ps_enabled_ = false;
  std::string current_ssid_;
  uint8_t last_reason_ = 0;
  ApHint fast_hint_;
  ApHint last_ap_;

  EventGroupHandle_t event_group_ = nullptr;
  volatile bool connecting_ = false;
  bool fast_hint_used_ = false;

  SemaphoreHandle_t radio_mutex_ = nullptr;
  mutable SemaphoreHandle_t diag_mutex_ = nullptr;
  WifiDiagLog diag_;
  ApFallbackPolicy fallback_policy_;

  WifiLinkStats link_stats_;
  WifiRoamPolicy roam_policy_;
  std::atomic<int64_t> roam_defer_until_ms_{0};
  volatile uint32_t beacon_timeouts_ = 0;
  uint32_t roams_ = 0;
  uint32_t roam_scans_ = 0;
  int32_t last_assoc_ms_ = -1;
  int32_t last_dhcp_ms_ = -1;
  int8_t tx_power_qdbm_ = 0;
  volatile uint32_t assoc_at_ms_ = 0;
  volatile bool relink_pending_ = false;

  void setState(WifiState s);
  void applyPowerSave();
  void checkTxPower(const char* when);
  void waitRfQuiet(const char* what);
  esp_err_t ensureStarted();
  bool scanLocked(std::vector<WifiScanHit>* hits);
  void diagAttempt(const std::string& ssid);
  void diagResult(const std::string& ssid, WifiAttemptResult result, uint8_t reason);
  static void wifiEventHandler(void* arg, esp_event_base_t base, int32_t id, void* data);
};

}  // namespace eego::net
