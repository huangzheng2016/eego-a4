// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

//
//
//

#include <cstdint>
#include <atomic>
#include <mutex>
#include <string>

#include "esp_err.h"
#include "esp_event.h"

namespace eego::admin {
class ProvisionPortal {
 public:
  static constexpr const char* kApSsid = "EEGO-A4-Setup";
  static constexpr const char* kApUrl = "http://192.168.4.1/";

  static ProvisionPortal& instance();

  esp_err_t open();
  void close(const char* reason);  // "expired" | "success" | "manual"
  bool isOpen() const { return portal_running_; }
  uint32_t windowRemainingMs() const;

  esp_err_t openApFallback();
  void closeApFallback(const char* reason);
  bool isApFallbackActive() const { return fallback_running_; }

  bool apActive() const { return portal_running_ || fallback_running_; }
  int apClientCount() const;

  void notePortalActivity();

  enum class ProvState : uint8_t { Idle = 0, Connecting, Ok, Fail };
  ProvState provState() const { return prov_state_; }
  const std::string& provReason() const { return prov_reason_; }
  const std::string& provIp() const { return prov_ip_; }
  int provReasonCode() const { return prov_reason_code_.load(); }
  std::string provIpCopy() const {
    std::lock_guard<std::mutex> lock(prov_mutex_);
    return prov_ip_;
  }
  std::string provSsidCopy() const {
    std::lock_guard<std::mutex> lock(prov_mutex_);
    return prov_ssid_;
  }

  static void wifiEventHandler(void* arg, esp_event_base_t base, int32_t id, void* data);

  ProvState prov_state_ = ProvState::Idle;
  std::string prov_reason_;
  std::string prov_ip_;
  std::string prov_ssid_;
  std::string prov_wifi_pass_;
  std::string prov_admin_pass_;
  int64_t connect_start_us_ = 0;
  std::atomic<int> prov_reason_code_{-1};
  mutable std::mutex prov_mutex_;
  void* connect_timer_ = nullptr;

  void checkConnectTimeout();
  void onConnectSucceeded(const std::string& ip);
  void onConnectFailed(const char* reason, int code = -1);
  void armConnectTimer();

 private:
  ProvisionPortal() = default;

  bool portal_running_ = false;
  bool fallback_running_ = false;
  bool ap_netif_created_ = false;
  bool sta_handler_registered_ = false;

  void* window_timer_ = nullptr;  // esp_timer_handle_t
  int64_t window_deadline_ms_ = 0;

  esp_err_t startAp();
  void stopAp();
  void startWindowTimer(bool idle_reset);
  void stopWindowTimer();
};

}  // namespace eego::admin
