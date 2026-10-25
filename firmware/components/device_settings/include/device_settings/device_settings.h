// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Versioned, non-secret device preferences. Secrets never enter this component.
// Writes and subscriber callbacks run in the caller's context; the admin HTTP
// path queues writes to app_task. A successful NVS commit precedes every notify.

#include <cstdint>
#include <string>

#include "device_settings/settings_model.h"
#include "esp_err.h"

namespace eego::settings {

struct Capabilities {
  bool frontlight = false;
  // LM3630A chip detected by the hardware probe, independent of the
  // frontlight_hardware override (used to resolve FrontlightHardware::Auto).
  bool frontlight_chip = false;
  bool battery = false;
  bool bluetooth = false;
  bool ble_provisioning = false;
  bool ble_observer = false;
  bool display = false;
  bool sd = false;
  bool rtc = false;
};

struct Snapshot {
  Settings values;
  Capabilities capabilities;
  uint32_t revision = 0;
  bool migrated = false;
  bool loaded = false;
};

class DeviceSettings {
 public:
  using Callback = void (*)(const Settings& values, uint32_t changed, void* ctx);

  static DeviceSettings& instance();

  // Loads the versioned record. If absent, migrates NVS "ui" preferences and
  // the supplied admin.display object, then persists before erasing legacy keys.
  esp_err_t init(const alert_core::JsonValue* legacy_admin_display);

  Snapshot snapshot() const;
  void setCapabilities(const Capabilities& capabilities);

  // Validates and atomically persists the complete candidate. RAM and
  // subscribers are unchanged on failure. changed may be null.
  bool update(const Settings& candidate, uint32_t* changed, std::string& error);

  int subscribe(Callback callback, void* ctx);  // -1 when the fixed table is full
  void unsubscribe(int id);

  // Factory recovery only; does not notify because the caller reboots.
  static esp_err_t erasePersisted();

 private:
  DeviceSettings() = default;
  void ensureMutex() const;

  mutable void* mutex_ = nullptr;
  Settings values_;
  Capabilities capabilities_;
  uint32_t revision_ = 0;
  bool migrated_ = false;
  bool loaded_ = false;

  struct Slot {
    int id = 0;
    Callback callback = nullptr;
    void* ctx = nullptr;
  };
  Slot slots_[6]{};
  int next_subscription_ = 1;
};

}  // namespace eego::settings
