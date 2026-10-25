// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "alert_core/json.h"

namespace eego::settings {

inline constexpr int64_t kSchemaVersion = 1;

enum class PowerMode : uint8_t {
  Auto = 0,
  Mains = 1,
  Battery = 2,
};

// 前光硬件覆盖：auto=按 LM3630A 探测；present/absent=强制可用/不可用。
enum class FrontlightHardware : uint8_t {
  Auto = 0,
  Present = 1,
  Absent = 2,
};

enum Change : uint32_t {
  kChangedLanguage = 1u << 0,
  kChangedPower = 1u << 1,
  kChangedFrontlight = 1u << 2,
  kChangedDataRefresh = 1u << 3,
  kChangedSleep = 1u << 4,
  kChangedTimezone = 1u << 5,
  kChangedDiagnostics = 1u << 6,
  kChangedEpdRefresh = 1u << 7,
  kChangedBle = 1u << 8,
  kChangedFrontlightHardware = 1u << 9,
  kChangedRefresh = kChangedDataRefresh | kChangedEpdRefresh,
  kChangedAll = (1u << 10) - 1,
};

inline constexpr size_t kMaxBleObserverRules = 4;
inline constexpr size_t kMaxBleObserverFields = 4;
inline constexpr size_t kMaxBlePayloadMatchBytes = 12;

enum class BleValueType : uint8_t {
  UInt8 = 0,
  Int8,
  UInt16Le,
  Int16Le,
  UInt32Le,
  Int32Le,
  Float32Le,
};

struct BleFieldMap {
  std::string name;
  uint16_t offset = 0;
  BleValueType type = BleValueType::UInt16Le;
  double scale = 1.0;
};

struct BleObserverRule {
  std::string id;
  std::string event_type = "ble.sensor";
  std::string title = "BLE sensor";
  int manufacturer_id = -1;
  std::string service_uuid;
  uint16_t payload_offset = 0;
  std::vector<uint8_t> payload_match;
  std::vector<BleFieldMap> fields;
  uint32_t ttl_s = 86400;
};

struct Settings {
  std::string language = "zh";
  PowerMode power_mode = PowerMode::Auto;
  uint32_t battery_poll_s = 900;
  uint32_t data_refresh_s = 0;
  uint32_t idle_sleep_s = 90;
  int sleep_start_min = -1;
  int sleep_end_min = -1;
  int timezone_offset_min = 480;
  bool frontlight_enabled = false;
  uint8_t frontlight_brightness = 60;
  uint8_t frontlight_warmth = 50;
  FrontlightHardware frontlight_hardware = FrontlightHardware::Auto;
  uint32_t epd_full_refresh_s = 86400;
  bool diagnostics_enabled = true;
  bool ble_observer_enabled = false;
  uint32_t ble_observer_window_s = 10;
  uint32_t ble_observer_interval_s = 300;
  uint32_t ble_observer_dedup_ms = 5000;
  std::vector<BleObserverRule> ble_observer_rules;
};

bool operator==(const BleFieldMap& a, const BleFieldMap& b);
bool operator==(const BleObserverRule& a, const BleObserverRule& b);
const char* bleValueTypeName(BleValueType type);
bool bleValueTypeFromString(const std::string& name, BleValueType& out);
bool normalizeBleUuid(const std::string& text, std::string& canonical);
std::string bleBytesToHex(const std::vector<uint8_t>& bytes);
bool bleBytesFromHex(const std::string& text, std::vector<uint8_t>& out);

struct FieldError {
  std::string field;
  std::string message;
};
using FieldErrors = std::vector<FieldError>;

const char* powerModeName(PowerMode mode);
bool powerModeFromString(const std::string& name, PowerMode& out);
const char* frontlightHardwareName(FrontlightHardware mode);
bool frontlightHardwareFromString(const std::string& name, FrontlightHardware& out);

alert_core::JsonValue toJson(const Settings& settings);
void validate(const Settings& settings, FieldErrors& errors);

// Applies a strict, partial typed patch. schema_version=1 is accepted as read-only
// metadata; every other unknown field is rejected. out is unchanged on failure.
bool mergePatch(const Settings& current, const alert_core::JsonValue& patch,
                Settings& out, FieldErrors& errors);

uint32_t changedFields(const Settings& before, const Settings& after);

// One-way migration/legacy write path for admin.display. Brightness zero means
// disabled; a nonzero legacy brightness preserves the old power-on behavior.
void applyLegacyDisplay(const alert_core::JsonValue& display, Settings& settings);
void validateLegacyDisplayCapability(const alert_core::JsonValue& display_patch,
                                     const Settings& current, bool frontlight_available,
                                     FieldErrors& errors);

// Daily local-time deep-sleep window, including overnight windows. A disabled
// schedule or invalid `now_unix` returns false. The wake timestamp is exact
// when the device is currently inside the configured window; otherwise zero.
bool scheduledSleep(const Settings& settings, uint64_t now_unix,
                    uint64_t& wake_at_unix, uint32_t& remaining_s);

const char* schemaJson();

}  // namespace eego::settings
