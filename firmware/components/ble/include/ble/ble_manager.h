// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "ble/observer_core.h"
#include "device_settings/settings_model.h"
#include "esp_err.h"
#include "host/ble_gap.h"
#include "wifi_provisioning/manager.h"

namespace eego::ble {

enum class Role : uint8_t {
  Off = 0,
  Observer,
  Provisioning,
};

enum class ProvisionState : uint8_t {
  Idle = 0,
  Active,
  Succeeded,
  Failed,
  TimedOut,
  Cancelled,
};

struct Status {
  bool compiled = false;
  bool stack_active = false;
  bool recovery_ap_active = false;
  Role role = Role::Off;
  ProvisionState provisioning = ProvisionState::Idle;
  uint32_t provisioning_remaining_s = 0;
  uint32_t observer_remaining_s = 0;
  uint32_t dropped_advertisements = 0;
  uint32_t emitted_events = 0;
  uint32_t duplicate_advertisements = 0;
  uint32_t malformed_advertisements = 0;
  uint32_t ingest_errors = 0;
  uint32_t diram_free_bytes = 0;
  uint32_t diram_min_free_bytes = 0;
  uint32_t psram_free_bytes = 0;
  std::string last_error;
};

using CredentialSink = esp_err_t (*)(const char* ssid, const char* password, void* ctx);

class BleManager {
 public:
  static BleManager& instance();
  static bool compiled();

  esp_err_t init(CredentialSink sink, void* sink_ctx);
  void configure(const eego::settings::Settings& settings);
  void onPowerMode(bool mains);
  void setBatteryScanWindow(bool open);
  void service(bool mains);

  esp_err_t startProvisioningFromLocal(const std::string& pop);
  void cancelProvisioning();
  bool provisioningActive() const;
  bool sleepBlocked() const;
  esp_err_t beforeSleep();

  bool beginRecoveryAp();
  void endRecoveryAp();

  esp_err_t clearBonds();
  Status status() const;

 private:
  BleManager() = default;
  void lock() const;
  void unlock() const;
  esp_err_t startObserverLocked();
  bool stopObserverLocked();
  void stopProvisioningLocked(ProvisionState terminal);
  void drainAdvertisementsLocked();
  void drainProvisioningLocked();
  void setErrorLocked(const std::string& error);
  void updateMemoryStatusLocked();

  struct RawAdvertisement;
  struct ProvisionMessage;

  mutable void* mutex_ = nullptr;
  void* adv_queue_ = nullptr;
  void* prov_queue_ = nullptr;
  CredentialSink credential_sink_ = nullptr;
  void* credential_sink_ctx_ = nullptr;

  eego::settings::Settings settings_{};
  ObserverCore observer_;
  std::atomic<Role> role_{Role::Off};
  ProvisionState provision_state_ = ProvisionState::Idle;
  std::string last_error_;
  std::string pop_;
  std::string service_name_;
  int64_t provisioning_deadline_ms_ = 0;
  int64_t provisioning_success_ms_ = 0;
  int64_t observer_started_ms_ = 0;
  int64_t observer_end_ms_ = 0;
  int64_t next_observer_ms_ = 0;
  int64_t stop_retry_ms_ = 0;
  int64_t memory_sample_ms_ = 0;
  bool mains_ = true;
  bool battery_window_open_ = false;
  bool battery_scan_done_ = false;
  std::atomic<bool> observer_synced_{false};
  bool observer_stop_pending_ = false;
  bool provisioning_credentials_saved_ = false;
  bool provisioning_credentials_failed_ = false;
  std::atomic<bool> provisioning_transport_fault_{false};
  bool provisioning_success_ = false;
  bool provisioning_end_ = false;
  std::atomic<bool> observer_disc_complete_{false};
  std::atomic<bool> observer_failed_{false};
  std::atomic<bool> recovery_ap_active_{false};

  std::atomic<uint32_t> dropped_advertisements_{0};
  uint32_t emitted_events_ = 0;
  uint32_t duplicate_advertisements_ = 0;
  uint32_t malformed_advertisements_ = 0;
  uint32_t ingest_errors_ = 0;
  uint32_t diram_free_bytes_ = 0;
  uint32_t diram_min_free_bytes_ = 0;
  uint32_t psram_free_bytes_ = 0;

  static void hostTask(void* arg);
  static void onSync();
  static void onReset(int reason);
  static int onGapEvent(struct ble_gap_event* event, void* arg);
  static void onProvisioningEvent(void* arg, wifi_prov_cb_event_t event, void* event_data);
};

const char* roleName(Role role);
const char* provisionStateName(ProvisionState state);

}  // namespace eego::ble
