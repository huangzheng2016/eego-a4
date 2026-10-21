// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

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

enum Change : uint32_t {
  kChangedLanguage = 1u << 0,
  kChangedPower = 1u << 1,
  kChangedFrontlight = 1u << 2,
  kChangedDataRefresh = 1u << 3,
  kChangedSleep = 1u << 4,
  kChangedTimezone = 1u << 5,
  kChangedDiagnostics = 1u << 6,
  kChangedEpdRefresh = 1u << 7,
  kChangedRefresh = kChangedDataRefresh | kChangedEpdRefresh,
  kChangedAll = (1u << 8) - 1,
};

struct Settings {
  std::string language = "zh";
  PowerMode power_mode = PowerMode::Auto;
  uint32_t battery_poll_s = 900;
  uint32_t data_refresh_s = 0;
  uint32_t idle_sleep_s = 90;
  int sleep_start_min = -1;
  int sleep_end_min = -1;
  int timezone_offset_min = 0;
  bool frontlight_enabled = false;
  uint8_t frontlight_brightness = 60;
  uint8_t frontlight_warmth = 50;
  uint32_t epd_full_refresh_s = 86400;
  bool diagnostics_enabled = true;
};

struct FieldError {
  std::string field;
  std::string message;
};
using FieldErrors = std::vector<FieldError>;

const char* powerModeName(PowerMode mode);
bool powerModeFromString(const std::string& name, PowerMode& out);

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
