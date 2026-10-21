// SPDX-License-Identifier: GPL-3.0-or-later
#include "device_settings/settings_model.h"

namespace eego::settings {

namespace {

using alert_core::JsonValue;

void addError(FieldErrors& errors, const std::string& field, const std::string& message) {
  errors.push_back({field, message});
}

bool isInt(const JsonValue& value) { return value.type() == JsonValue::Type::Int; }

bool readBool(const JsonValue& value, const char* field, bool& out, FieldErrors& errors) {
  if (value.type() != JsonValue::Type::Bool) {
    addError(errors, field, "must be a boolean");
    return false;
  }
  out = value.boolValue();
  return true;
}

bool readString(const JsonValue& value, const char* field, std::string& out,
                FieldErrors& errors) {
  if (value.type() != JsonValue::Type::String) {
    addError(errors, field, "must be a string");
    return false;
  }
  out = value.stringValue();
  return true;
}

}  // namespace

const char* powerModeName(PowerMode mode) {
  switch (mode) {
    case PowerMode::Mains: return "mains";
    case PowerMode::Battery: return "battery";
    default: return "auto";
  }
}

bool powerModeFromString(const std::string& name, PowerMode& out) {
  if (name == "auto") out = PowerMode::Auto;
  else if (name == "mains") out = PowerMode::Mains;
  else if (name == "battery") out = PowerMode::Battery;
  else return false;
  return true;
}

JsonValue toJson(const Settings& s) {
  return JsonValue::makeObject({
      {"schema_version", JsonValue::makeInt(kSchemaVersion)},
      {"language", JsonValue::makeString(s.language)},
      {"power_mode", JsonValue::makeString(powerModeName(s.power_mode))},
      {"battery_poll_s", JsonValue::makeInt(s.battery_poll_s)},
      {"data_refresh_s", JsonValue::makeInt(s.data_refresh_s)},
      {"idle_sleep_s", JsonValue::makeInt(s.idle_sleep_s)},
      {"sleep_start_min", JsonValue::makeInt(s.sleep_start_min)},
      {"sleep_end_min", JsonValue::makeInt(s.sleep_end_min)},
      {"timezone_offset_min", JsonValue::makeInt(s.timezone_offset_min)},
      {"frontlight_enabled", JsonValue::makeBool(s.frontlight_enabled)},
      {"frontlight_brightness", JsonValue::makeInt(s.frontlight_brightness)},
      {"frontlight_warmth", JsonValue::makeInt(s.frontlight_warmth)},
      {"epd_full_refresh_s", JsonValue::makeInt(s.epd_full_refresh_s)},
      {"diagnostics_enabled", JsonValue::makeBool(s.diagnostics_enabled)},
  });
}

void validate(const Settings& s, FieldErrors& errors) {
  if (s.language != "zh" && s.language != "en") {
    addError(errors, "language", "must be one of zh|en");
  }
  if (static_cast<uint8_t>(s.power_mode) > static_cast<uint8_t>(PowerMode::Battery)) {
    addError(errors, "power_mode", "must be one of auto|mains|battery");
  }
  if (s.battery_poll_s < 60 || s.battery_poll_s > 21600) {
    addError(errors, "battery_poll_s", "must be in [60, 21600]");
  }
  if (s.data_refresh_s != 0 && (s.data_refresh_s < 30 || s.data_refresh_s > 86400)) {
    addError(errors, "data_refresh_s", "must be 0 (source schedules) or in [30, 86400]");
  }
  if (s.idle_sleep_s != 0 && (s.idle_sleep_s < 30 || s.idle_sleep_s > 600)) {
    addError(errors, "idle_sleep_s", "must be 0 (disabled) or in [30, 600]");
  }
  const bool startOff = s.sleep_start_min == -1;
  const bool endOff = s.sleep_end_min == -1;
  if (startOff != endOff) {
    addError(errors, "sleep_start_min", "sleep start and end must both be -1 or both be set");
  } else if (!startOff) {
    if (s.sleep_start_min < 0 || s.sleep_start_min > 1439) {
      addError(errors, "sleep_start_min", "must be -1 or in [0, 1439]");
    }
    if (s.sleep_end_min < 0 || s.sleep_end_min > 1439) {
      addError(errors, "sleep_end_min", "must be -1 or in [0, 1439]");
    }
    if (s.sleep_start_min == s.sleep_end_min) {
      addError(errors, "sleep_end_min", "sleep start and end must differ");
    }
  }
  if (s.timezone_offset_min < -840 || s.timezone_offset_min > 840 ||
      s.timezone_offset_min % 15 != 0) {
    addError(errors, "timezone_offset_min", "must be a 15-minute step in [-840, 840]");
  }
  if (s.frontlight_brightness < 1 || s.frontlight_brightness > 100) {
    addError(errors, "frontlight_brightness", "must be in [1, 100]");
  }
  if (s.frontlight_warmth > 100) {
    addError(errors, "frontlight_warmth", "must be in [0, 100]");
  }
  if (s.epd_full_refresh_s != 0 &&
      (s.epd_full_refresh_s < 3600 || s.epd_full_refresh_s > 604800)) {
    addError(errors, "epd_full_refresh_s", "must be 0 (manual) or in [3600, 604800]");
  }
}

bool mergePatch(const Settings& current, const JsonValue& patch, Settings& out,
                FieldErrors& errors) {
  if (patch.type() != JsonValue::Type::Object) {
    addError(errors, "", "settings patch must be an object");
    return false;
  }
  Settings next = current;
  for (const auto& kv : patch.objectItems()) {
    const std::string& key = kv.first;
    const JsonValue& value = kv.second;
    if (key == "schema_version") {
      if (!isInt(value) || value.intValue() != kSchemaVersion) {
        addError(errors, key, "unsupported schema_version (expected 1)");
      }
    } else if (key == "language") {
      std::string text;
      if (readString(value, key.c_str(), text, errors)) next.language = text;
    } else if (key == "power_mode") {
      std::string text;
      if (readString(value, key.c_str(), text, errors)) {
        PowerMode mode;
        if (!powerModeFromString(text, mode)) {
          addError(errors, key, "must be one of auto|mains|battery");
        } else {
          next.power_mode = mode;
        }
      }
    } else if (key == "battery_poll_s" || key == "data_refresh_s" ||
               key == "idle_sleep_s" || key == "sleep_start_min" ||
               key == "sleep_end_min" || key == "timezone_offset_min" ||
               key == "frontlight_brightness" || key == "frontlight_warmth" ||
               key == "epd_full_refresh_s") {
      int64_t number = 0;
      if (!isInt(value)) {
        addError(errors, key, "must be an integer");
        continue;
      }
      number = value.intValue();
      if (key == "battery_poll_s" && number >= 0 && number <= UINT32_MAX) {
        next.battery_poll_s = static_cast<uint32_t>(number);
      } else if (key == "data_refresh_s" && number >= 0 && number <= UINT32_MAX) {
        next.data_refresh_s = static_cast<uint32_t>(number);
      } else if (key == "idle_sleep_s" && number >= 0 && number <= UINT32_MAX) {
        next.idle_sleep_s = static_cast<uint32_t>(number);
      } else if (key == "sleep_start_min" && number >= -1 && number <= INT32_MAX) {
        next.sleep_start_min = static_cast<int>(number);
      } else if (key == "sleep_start_min" || key == "sleep_end_min") {
        if (number >= -1 && number <= INT32_MAX) {
          if (key == "sleep_start_min") next.sleep_start_min = static_cast<int>(number);
          else next.sleep_end_min = static_cast<int>(number);
        } else {
          addError(errors, key, "integer is out of range");
        }
      } else if (key == "timezone_offset_min" && number >= INT32_MIN && number <= INT32_MAX) {
        next.timezone_offset_min = static_cast<int>(number);
      } else if ((key == "frontlight_brightness" || key == "frontlight_warmth") &&
                 number >= 0 && number <= 255) {
        if (key == "frontlight_brightness") next.frontlight_brightness = static_cast<uint8_t>(number);
        else next.frontlight_warmth = static_cast<uint8_t>(number);
      } else if (key == "epd_full_refresh_s" && number >= 0 && number <= UINT32_MAX) {
        next.epd_full_refresh_s = static_cast<uint32_t>(number);
      } else {
        addError(errors, key, "integer is out of range");
      }
    } else if (key == "frontlight_enabled") {
      readBool(value, key.c_str(), next.frontlight_enabled, errors);
    } else if (key == "diagnostics_enabled") {
      readBool(value, key.c_str(), next.diagnostics_enabled, errors);
    } else {
      addError(errors, key, "unknown or read-only field");
    }
  }
  validate(next, errors);
  if (!errors.empty()) return false;
  out = next;
  return true;
}

uint32_t changedFields(const Settings& a, const Settings& b) {
  uint32_t changed = 0;
  if (a.language != b.language) changed |= kChangedLanguage;
  if (a.power_mode != b.power_mode || a.battery_poll_s != b.battery_poll_s) {
    changed |= kChangedPower;
  }
  if (a.frontlight_enabled != b.frontlight_enabled ||
      a.frontlight_brightness != b.frontlight_brightness ||
      a.frontlight_warmth != b.frontlight_warmth) {
    changed |= kChangedFrontlight;
  }
  if (a.data_refresh_s != b.data_refresh_s) changed |= kChangedDataRefresh;
  if (a.epd_full_refresh_s != b.epd_full_refresh_s) changed |= kChangedEpdRefresh;
  if (a.idle_sleep_s != b.idle_sleep_s || a.sleep_start_min != b.sleep_start_min ||
      a.sleep_end_min != b.sleep_end_min) {
    changed |= kChangedSleep;
  }
  if (a.timezone_offset_min != b.timezone_offset_min) changed |= kChangedTimezone;
  if (a.diagnostics_enabled != b.diagnostics_enabled) changed |= kChangedDiagnostics;
  return changed;
}

void applyLegacyDisplay(const JsonValue& display, Settings& settings) {
  if (display.type() != JsonValue::Type::Object) return;
  if (const JsonValue* value = display.find("brightness");
      value != nullptr && isInt(*value) && value->intValue() >= 0 && value->intValue() <= 100) {
    const int brightness = static_cast<int>(value->intValue());
    settings.frontlight_enabled = brightness > 0;
    if (brightness > 0) settings.frontlight_brightness = static_cast<uint8_t>(brightness);
  }
  if (const JsonValue* value = display.find("warmth");
      value != nullptr && isInt(*value) && value->intValue() >= 0 && value->intValue() <= 100) {
    settings.frontlight_warmth = static_cast<uint8_t>(value->intValue());
  }
}

void validateLegacyDisplayCapability(const JsonValue& display_patch, const Settings& current,
                                     bool frontlight_available, FieldErrors& errors) {
  if (frontlight_available || display_patch.type() != JsonValue::Type::Object) return;
  const int current_brightness = current.frontlight_enabled ? current.frontlight_brightness : 0;
  if (const JsonValue* value = display_patch.find("brightness");
      value != nullptr && isInt(*value) && value->intValue() != current_brightness) {
    addError(errors, "admin.display.brightness", "frontlight hardware is unavailable");
  }
  if (const JsonValue* value = display_patch.find("warmth");
      value != nullptr && isInt(*value) && value->intValue() != current.frontlight_warmth) {
    addError(errors, "admin.display.warmth", "frontlight hardware is unavailable");
  }
}

bool scheduledSleep(const Settings& settings, uint64_t now_unix, uint64_t& wake_at_unix,
                    uint32_t& remaining_s) {
  wake_at_unix = 0;
  remaining_s = 0;
  if (now_unix < 946684800ULL || settings.sleep_start_min < 0 || settings.sleep_end_min < 0 ||
      settings.sleep_start_min == settings.sleep_end_min) {
    return false;
  }
  const int64_t local = static_cast<int64_t>(now_unix) +
                        static_cast<int64_t>(settings.timezone_offset_min) * 60;
  int minute = static_cast<int>((local / 60) % 1440);
  if (minute < 0) minute += 1440;
  const int start = settings.sleep_start_min;
  const int end = settings.sleep_end_min;
  const bool active = start < end ? (minute >= start && minute < end)
                                  : (minute >= start || minute < end);
  if (!active) return false;
  int until_end = 0;
  if (start < end) {
    until_end = (end - minute) * 60;
  } else if (minute >= start) {
    until_end = (1440 - minute + end) * 60;
  } else {
    until_end = (end - minute) * 60;
  }
  int second = static_cast<int>(local % 60);
  if (second < 0) second += 60;
  until_end -= second;
  if (until_end <= 0) return false;
  remaining_s = static_cast<uint32_t>(until_end);
  wake_at_unix = now_unix + remaining_s;
  return true;
}

const char* schemaJson() {
  return R"JSON({
  "schema_version": 1,
  "description": "Versioned non-secret local device preferences. PUT /api/v1/settings accepts a partial typed object; secrets remain in NVS sec/wifi namespaces.",
  "fields": {
    "language": {"type":"string","enum":["zh","en"],"default":"zh","group":"time_language"},
    "power_mode": {"type":"string","enum":["auto","mains","battery"],"default":"auto","group":"power"},
    "battery_poll_s": {"type":"int","range":[60,21600],"default":900,"group":"power"},
    "data_refresh_s": {"type":"int","range":[0,86400],"minimum_nonzero":30,"default":0,"note":"0 follows source schedules","group":"refresh"},
    "idle_sleep_s": {"type":"int","range":[0,600],"minimum_nonzero":30,"default":90,"note":"0 disables idle deep sleep","group":"power"},
    "sleep_start_min": {"type":"int","range":[-1,1439],"default":-1,"note":"local daily schedule; -1 with sleep_end_min=-1 disables","group":"power"},
    "sleep_end_min": {"type":"int","range":[-1,1439],"default":-1,"group":"power"},
    "timezone_offset_min": {"type":"int","range":[-840,840],"step":15,"default":0,"group":"time_language"},
    "frontlight_enabled": {"type":"bool","default":false,"capability":"frontlight","group":"display"},
    "frontlight_brightness": {"type":"int","range":[1,100],"default":60,"capability":"frontlight","group":"display"},
    "frontlight_warmth": {"type":"int","range":[0,100],"default":50,"capability":"frontlight","group":"display"},
    "epd_full_refresh_s": {"type":"int","range":[0,604800],"minimum_nonzero":3600,"default":86400,"note":"0 disables periodic full refresh","group":"refresh"},
    "diagnostics_enabled": {"type":"bool","default":true,"group":"diagnostics"}
  },
  "deep_sleep": {"http_reachable":false,"ble_reachable":false,"note":"Recovery AP and provisioning pause automatic sleep."}
})JSON";
}

}  // namespace eego::settings
