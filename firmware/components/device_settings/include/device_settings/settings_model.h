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

enum class FrontlightHardware : uint8_t {
  Auto = 0,
  Present = 1,
  Absent = 2,
};

enum class ChargeSignal : uint8_t {
  Auto = 0,
  ActiveHigh = 1,
  ActiveLow = 2,
};

// Which link carries outbound HTTP for data sources.
enum class UplinkMode : uint8_t {
  Auto = 0,  // the computer agent over BLE when linked, otherwise WiFi
  Wifi = 1,
  Ble = 2,
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
  kChangedIdleRefresh = 1u << 10,  // idle_after_min / idle_refresh_min / idle_critical_immediate / keep_awake_min
  kChangedBattery = 1u << 11,      // battery_refresh_min / battery_max_stale_min / battery_interactive_timeout_s
  kChangedScreenRotate = 1u << 12, // screen_rotate_min
  kChangedChargeSignal = 1u << 13,
  kChangedUplink = 1u << 14,
  kChangedRefresh = kChangedDataRefresh | kChangedEpdRefresh,
  kChangedAll = (1u << 15) - 1,
};

inline constexpr uint32_t kIdleRefreshChoices[] = {2, 5, 10, 15, 30, 60};
inline constexpr uint32_t kKeepAwakeChoices[] = {10, 30, 60, 120};
inline constexpr uint32_t kScreenRotateChoices[] = {0, 1, 2, 5, 10, 30};
inline constexpr uint32_t kIdleAfterMaxMin = 1440;

inline constexpr uint32_t kBatteryRefreshChoices[] = {5, 10, 15, 30, 60};
inline constexpr uint32_t kBatteryMaxStaleChoices[] = {15, 30, 60, 120, 240, 480};
inline constexpr uint32_t kBatteryInteractiveMinS = 20;
inline constexpr uint32_t kBatteryInteractiveMaxS = 600;

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
  ChargeSignal charge_signal = ChargeSignal::Auto;
  uint32_t data_refresh_s = 0;
  uint32_t battery_refresh_min = 15;
  uint32_t battery_max_stale_min = 60;
  uint32_t battery_interactive_timeout_s = 60;
  int sleep_start_min = -1;
  int sleep_end_min = -1;
  int timezone_offset_min = 480;
  bool frontlight_enabled = false;
  uint8_t frontlight_brightness = 60;
  uint8_t frontlight_warmth = 50;
  FrontlightHardware frontlight_hardware = FrontlightHardware::Auto;
  uint32_t epd_full_refresh_s = 86400;
  uint32_t idle_after_min = 10;
  uint32_t idle_refresh_min = 5;
  bool idle_critical_immediate = true;
  uint32_t keep_awake_min = 30;
  uint32_t screen_rotate_min = 0;
  bool diagnostics_enabled = true;
  bool ble_admin_enabled = true;
  UplinkMode uplink = UplinkMode::Auto;
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
const char* chargeSignalName(ChargeSignal mode);
bool chargeSignalFromString(const std::string& name, ChargeSignal& out);
const char* uplinkModeName(UplinkMode mode);
bool uplinkModeFromString(const std::string& name, UplinkMode& out);

alert_core::JsonValue toJson(const Settings& settings);
void validate(const Settings& settings, FieldErrors& errors);

// Applies a strict, partial typed patch. schema_version=1 is accepted as read-only
// metadata; every other unknown field is rejected. out is unchanged on failure.
bool mergePatch(const Settings& current, const alert_core::JsonValue& patch,
                Settings& out, FieldErrors& errors);

uint32_t changedFields(const Settings& before, const Settings& after);

bool validIdleRefreshMin(uint32_t minutes);
bool validKeepAwakeMin(uint32_t minutes);
bool validScreenRotateMin(uint32_t minutes);
bool validBatteryRefreshMin(uint32_t minutes);
bool validBatteryMaxStaleMin(uint32_t minutes);

// Daily local-time deep-sleep window, including overnight windows. A disabled
// schedule or invalid `now_unix` returns false. The wake timestamp is exact
// when the device is currently inside the configured window; otherwise zero.
bool scheduledSleep(const Settings& settings, uint64_t now_unix,
                    uint64_t& wake_at_unix, uint32_t& remaining_s);

const char* schemaJson();

}  // namespace eego::settings
