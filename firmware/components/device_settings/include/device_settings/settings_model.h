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

// GPIO11 充电状态极性：auto=按 LM3630A 是否在位推断（在位 → 高有效）；high/low=强制。
// 推断只是经验规则，个别批次相反（表现为充电时显示电池、用电池时显示充电中）。
enum class ChargeSignal : uint8_t {
  Auto = 0,
  ActiveHigh = 1,
  ActiveLow = 2,
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
  kChangedRefresh = kChangedDataRefresh | kChangedEpdRefresh,
  kChangedAll = (1u << 14) - 1,
};

// 空闲显示刷新间隔（分钟）允许值：均整除 60，使边界落在整点对齐的墙钟时刻。
inline constexpr uint32_t kIdleRefreshChoices[] = {2, 5, 10, 15, 30, 60};
// 「保持唤醒」时长（分钟）：设备设置行/状态页软键/↑+↓ 一次开启的时长。
inline constexpr uint32_t kKeepAwakeChoices[] = {10, 30, 60, 120};
// dashboard 自动轮播间隔（分钟）：0 = 关；空闲时每隔这么久切到下一屏。
inline constexpr uint32_t kScreenRotateChoices[] = {0, 1, 2, 5, 10, 30};
inline constexpr uint32_t kIdleAfterMaxMin = 1440;

// 电池模式：唤醒间隔（分钟，均整除 60，唤醒对齐本地墙钟边界）与最长不刷屏时长。
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
  // 电池模式：每 battery_refresh_min 分钟（本地墙钟对齐）定时唤醒取数一次；帧内容
  // 不变时最长 battery_max_stale_min 分钟不刷屏（须 ≥ battery_refresh_min）；按键
  // 唤醒后 battery_interactive_timeout_s 秒无操作回到深睡。
  uint32_t battery_refresh_min = 15;
  uint32_t battery_max_stale_min = 60;
  uint32_t battery_interactive_timeout_s = 60;
  // 夜间暂停（本地分钟，-1 = 禁用）：窗口内不唤醒、不联网。
  int sleep_start_min = -1;
  int sleep_end_min = -1;
  int timezone_offset_min = 480;
  bool frontlight_enabled = false;
  uint8_t frontlight_brightness = 60;
  uint8_t frontlight_warmth = 50;
  FrontlightHardware frontlight_hardware = FrontlightHardware::Auto;
  uint32_t epd_full_refresh_s = 86400;
  // 空闲显示刷新：idle_after_min 分钟无交互（0 = 禁用）后，屏幕更新合并到每
  // idle_refresh_min 分钟的本地墙钟边界；新出现的活动严重告警可立即刷新。
  uint32_t idle_after_min = 10;
  uint32_t idle_refresh_min = 5;
  bool idle_critical_immediate = true;
  // 保持唤醒（配置模式）一次开启的时长；状态只在 RAM，到期自动结束。
  uint32_t keep_awake_min = 30;
  // dashboard 多屏自动轮播（分钟，0 = 关）：无交互满一个间隔后按墙钟对齐切到下一屏；
  // 空闲节奏下随空闲刷新显示，电池模式每次定时唤醒至多前进一屏。
  uint32_t screen_rotate_min = 0;
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
const char* chargeSignalName(ChargeSignal mode);
bool chargeSignalFromString(const std::string& name, ChargeSignal& out);

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
