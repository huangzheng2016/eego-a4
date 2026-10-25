// SPDX-License-Identifier: GPL-3.0-or-later
#include "device_settings/settings_model.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

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

bool readNumber(const JsonValue& value, double& out) {
  if (value.type() == JsonValue::Type::Int) {
    out = static_cast<double>(value.intValue());
    return true;
  }
  if (value.type() == JsonValue::Type::Double) {
    out = value.doubleValue();
    return true;
  }
  return false;
}

bool validIdentifier(const std::string& text, size_t max_len) {
  if (text.empty() || text.size() > max_len || !std::isalpha(static_cast<unsigned char>(text[0]))) {
    return false;
  }
  for (unsigned char c : text) {
    if (!std::isalnum(c) && c != '_' && c != '.' && c != '-') return false;
  }
  return true;
}

bool validTitle(const std::string& text) {
  if (text.empty() || text.size() > 48) return false;
  for (unsigned char c : text) {
    if (c < 0x20 || c == 0x7f) return false;
  }
  return true;
}

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

size_t bleValueWidth(BleValueType type) {
  switch (type) {
    case BleValueType::UInt8:
    case BleValueType::Int8: return 1;
    case BleValueType::UInt16Le:
    case BleValueType::Int16Le: return 2;
    case BleValueType::UInt32Le:
    case BleValueType::Int32Le:
    case BleValueType::Float32Le: return 4;
  }
  return 0;
}

JsonValue fieldToJson(const BleFieldMap& field) {
  return JsonValue::makeObject({
      {"name", JsonValue::makeString(field.name)},
      {"offset", JsonValue::makeInt(field.offset)},
      {"type", JsonValue::makeString(bleValueTypeName(field.type))},
      {"scale", JsonValue::makeDouble(field.scale)},
  });
}

JsonValue ruleToJson(const BleObserverRule& rule) {
  JsonValue::Array fields;
  for (const auto& field : rule.fields) fields.push_back(fieldToJson(field));
  JsonValue::Object object{
      {"id", JsonValue::makeString(rule.id)},
      {"event_type", JsonValue::makeString(rule.event_type)},
      {"title", JsonValue::makeString(rule.title)},
      {"payload_offset", JsonValue::makeInt(rule.payload_offset)},
      {"payload_match", JsonValue::makeString(bleBytesToHex(rule.payload_match))},
      {"fields", JsonValue::makeArray(std::move(fields))},
      {"ttl_s", JsonValue::makeInt(rule.ttl_s)},
  };
  if (rule.manufacturer_id >= 0) {
    object.emplace_back("manufacturer_id", JsonValue::makeInt(rule.manufacturer_id));
  } else {
    object.emplace_back("service_uuid", JsonValue::makeString(rule.service_uuid));
  }
  return JsonValue::makeObject(std::move(object));
}

bool parseField(const JsonValue& value, const std::string& path, BleFieldMap& out,
                FieldErrors& errors) {
  if (value.type() != JsonValue::Type::Object) {
    addError(errors, path, "must be an object");
    return false;
  }
  BleFieldMap field;
  for (const auto& kv : value.objectItems()) {
    const std::string field_path = path + "." + kv.first;
    if (kv.first == "name") {
      readString(kv.second, field_path.c_str(), field.name, errors);
    } else if (kv.first == "offset") {
      if (!isInt(kv.second) || kv.second.intValue() < 0 || kv.second.intValue() > 255) {
        addError(errors, field_path, "must be an integer in [0, 255]");
      } else {
        field.offset = static_cast<uint16_t>(kv.second.intValue());
      }
    } else if (kv.first == "type") {
      std::string name;
      if (readString(kv.second, field_path.c_str(), name, errors) &&
          !bleValueTypeFromString(name, field.type)) {
        addError(errors, field_path, "unsupported BLE value type");
      }
    } else if (kv.first == "scale") {
      if (!readNumber(kv.second, field.scale)) addError(errors, field_path, "must be a number");
    } else {
      addError(errors, field_path, "unknown field");
    }
  }
  if (!errors.empty()) return false;
  out = std::move(field);
  return true;
}

bool parseRule(const JsonValue& value, size_t index, BleObserverRule& out, FieldErrors& errors) {
  const std::string path = "ble_observer_rules[" + std::to_string(index) + "]";
  if (value.type() != JsonValue::Type::Object) {
    addError(errors, path, "must be an object");
    return false;
  }
  BleObserverRule rule;
  for (const auto& kv : value.objectItems()) {
    const std::string field_path = path + "." + kv.first;
    if (kv.first == "id" || kv.first == "event_type" || kv.first == "title") {
      std::string text;
      if (readString(kv.second, field_path.c_str(), text, errors)) {
        if (kv.first == "id") rule.id = std::move(text);
        else if (kv.first == "event_type") rule.event_type = std::move(text);
        else rule.title = std::move(text);
      }
    } else if (kv.first == "manufacturer_id") {
      if (!isInt(kv.second) || kv.second.intValue() < 0 || kv.second.intValue() > 65535) {
        addError(errors, field_path, "must be an integer in [0, 65535]");
      } else {
        rule.manufacturer_id = static_cast<int>(kv.second.intValue());
      }
    } else if (kv.first == "service_uuid") {
      std::string text;
      if (readString(kv.second, field_path.c_str(), text, errors)) {
        if (!normalizeBleUuid(text, rule.service_uuid)) {
          addError(errors, field_path, "must be a 16/32/128-bit UUID");
        }
      }
    } else if (kv.first == "payload_offset") {
      if (!isInt(kv.second) || kv.second.intValue() < 0 || kv.second.intValue() > 255) {
        addError(errors, field_path, "must be an integer in [0, 255]");
      } else {
        rule.payload_offset = static_cast<uint16_t>(kv.second.intValue());
      }
    } else if (kv.first == "payload_match") {
      std::string text;
      if (readString(kv.second, field_path.c_str(), text, errors) &&
          !bleBytesFromHex(text, rule.payload_match)) {
        addError(errors, field_path, "must be an even-length hexadecimal string");
      }
    } else if (kv.first == "fields") {
      if (kv.second.type() != JsonValue::Type::Array ||
          kv.second.arrayItems().size() > kMaxBleObserverFields) {
        addError(errors, field_path, "must be an array with at most 4 entries");
      } else {
        size_t field_index = 0;
        for (const auto& item : kv.second.arrayItems()) {
          BleFieldMap field;
          if (parseField(item, field_path + "[" + std::to_string(field_index) + "]", field,
                         errors)) {
            rule.fields.push_back(std::move(field));
          }
          field_index++;
        }
      }
    } else if (kv.first == "ttl_s") {
      if (!isInt(kv.second) || kv.second.intValue() < 0 || kv.second.intValue() > UINT32_MAX) {
        addError(errors, field_path, "must be an unsigned integer");
      } else {
        rule.ttl_s = static_cast<uint32_t>(kv.second.intValue());
      }
    } else {
      addError(errors, field_path, "unknown field");
    }
  }
  if (!errors.empty()) return false;
  out = std::move(rule);
  return true;
}

bool parseRules(const JsonValue& value, std::vector<BleObserverRule>& out, FieldErrors& errors) {
  if (value.type() != JsonValue::Type::Array || value.arrayItems().size() > kMaxBleObserverRules) {
    addError(errors, "ble_observer_rules", "must be an array with at most 4 entries");
    return false;
  }
  std::vector<BleObserverRule> rules;
  size_t index = 0;
  for (const auto& item : value.arrayItems()) {
    BleObserverRule rule;
    if (parseRule(item, index, rule, errors)) rules.push_back(std::move(rule));
    index++;
  }
  if (!errors.empty()) return false;
  out = std::move(rules);
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

const char* frontlightHardwareName(FrontlightHardware mode) {
  switch (mode) {
    case FrontlightHardware::Present: return "present";
    case FrontlightHardware::Absent: return "absent";
    default: return "auto";
  }
}

bool frontlightHardwareFromString(const std::string& name, FrontlightHardware& out) {
  if (name == "auto") out = FrontlightHardware::Auto;
  else if (name == "present") out = FrontlightHardware::Present;
  else if (name == "absent") out = FrontlightHardware::Absent;
  else return false;
  return true;
}

bool operator==(const BleFieldMap& a, const BleFieldMap& b) {
  return a.name == b.name && a.offset == b.offset && a.type == b.type && a.scale == b.scale;
}

bool operator==(const BleObserverRule& a, const BleObserverRule& b) {
  return a.id == b.id && a.event_type == b.event_type && a.title == b.title &&
         a.manufacturer_id == b.manufacturer_id && a.service_uuid == b.service_uuid &&
         a.payload_offset == b.payload_offset && a.payload_match == b.payload_match &&
         a.fields == b.fields && a.ttl_s == b.ttl_s;
}

const char* bleValueTypeName(BleValueType type) {
  switch (type) {
    case BleValueType::UInt8: return "u8";
    case BleValueType::Int8: return "i8";
    case BleValueType::UInt16Le: return "u16le";
    case BleValueType::Int16Le: return "i16le";
    case BleValueType::UInt32Le: return "u32le";
    case BleValueType::Int32Le: return "i32le";
    case BleValueType::Float32Le: return "f32le";
  }
  return "u16le";
}

bool bleValueTypeFromString(const std::string& name, BleValueType& out) {
  if (name == "u8") out = BleValueType::UInt8;
  else if (name == "i8") out = BleValueType::Int8;
  else if (name == "u16le") out = BleValueType::UInt16Le;
  else if (name == "i16le") out = BleValueType::Int16Le;
  else if (name == "u32le") out = BleValueType::UInt32Le;
  else if (name == "i32le") out = BleValueType::Int32Le;
  else if (name == "f32le") out = BleValueType::Float32Le;
  else return false;
  return true;
}

bool normalizeBleUuid(const std::string& text, std::string& canonical) {
  std::string hex;
  if (text.size() == 36) {
    if (text[8] != '-' || text[13] != '-' || text[18] != '-' || text[23] != '-') return false;
    for (size_t i = 0; i < text.size(); i++) {
      if (i == 8 || i == 13 || i == 18 || i == 23) continue;
      if (hexDigit(text[i]) < 0) return false;
      hex.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(text[i]))));
    }
  } else if (text.size() == 4 || text.size() == 8 || text.size() == 32) {
    for (char c : text) {
      if (hexDigit(c) < 0) return false;
      hex.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
  } else {
    return false;
  }
  if (hex.size() == 4) hex = "0000" + hex + "00001000800000805f9b34fb";
  if (hex.size() == 8) hex += "00001000800000805f9b34fb";
  if (hex.size() != 32) return false;
  canonical = hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" +
              hex.substr(16, 4) + "-" + hex.substr(20, 12);
  return true;
}

std::string bleBytesToHex(const std::vector<uint8_t>& bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (uint8_t byte : bytes) {
    out.push_back(kHex[byte >> 4]);
    out.push_back(kHex[byte & 0x0f]);
  }
  return out;
}

bool bleBytesFromHex(const std::string& text, std::vector<uint8_t>& out) {
  if ((text.size() & 1u) != 0) return false;
  std::vector<uint8_t> bytes;
  bytes.reserve(text.size() / 2);
  for (size_t i = 0; i < text.size(); i += 2) {
    const int high = hexDigit(text[i]);
    const int low = hexDigit(text[i + 1]);
    if (high < 0 || low < 0) return false;
    bytes.push_back(static_cast<uint8_t>((high << 4) | low));
  }
  out = std::move(bytes);
  return true;
}

JsonValue toJson(const Settings& s) {
  JsonValue::Array rules;
  for (const auto& rule : s.ble_observer_rules) rules.push_back(ruleToJson(rule));
  return JsonValue::makeObject({
      {"schema_version", JsonValue::makeInt(kSchemaVersion)},
      {"language", JsonValue::makeString(s.language)},
      {"power_mode", JsonValue::makeString(powerModeName(s.power_mode))},
      {"data_refresh_s", JsonValue::makeInt(s.data_refresh_s)},
      {"battery_refresh_min", JsonValue::makeInt(s.battery_refresh_min)},
      {"battery_max_stale_min", JsonValue::makeInt(s.battery_max_stale_min)},
      {"battery_interactive_timeout_s", JsonValue::makeInt(s.battery_interactive_timeout_s)},
      {"sleep_start_min", JsonValue::makeInt(s.sleep_start_min)},
      {"sleep_end_min", JsonValue::makeInt(s.sleep_end_min)},
      {"timezone_offset_min", JsonValue::makeInt(s.timezone_offset_min)},
      {"frontlight_enabled", JsonValue::makeBool(s.frontlight_enabled)},
      {"frontlight_brightness", JsonValue::makeInt(s.frontlight_brightness)},
      {"frontlight_warmth", JsonValue::makeInt(s.frontlight_warmth)},
      {"frontlight_hardware", JsonValue::makeString(frontlightHardwareName(s.frontlight_hardware))},
      {"epd_full_refresh_s", JsonValue::makeInt(s.epd_full_refresh_s)},
      {"idle_after_min", JsonValue::makeInt(s.idle_after_min)},
      {"idle_refresh_min", JsonValue::makeInt(s.idle_refresh_min)},
      {"idle_critical_immediate", JsonValue::makeBool(s.idle_critical_immediate)},
      {"diagnostics_enabled", JsonValue::makeBool(s.diagnostics_enabled)},
      {"ble_observer_enabled", JsonValue::makeBool(s.ble_observer_enabled)},
      {"ble_observer_window_s", JsonValue::makeInt(s.ble_observer_window_s)},
      {"ble_observer_interval_s", JsonValue::makeInt(s.ble_observer_interval_s)},
      {"ble_observer_dedup_ms", JsonValue::makeInt(s.ble_observer_dedup_ms)},
      {"ble_observer_rules", JsonValue::makeArray(std::move(rules))},
  });
}

void validate(const Settings& s, FieldErrors& errors) {
  if (s.language != "zh" && s.language != "en") {
    addError(errors, "language", "must be one of zh|en");
  }
  if (static_cast<uint8_t>(s.power_mode) > static_cast<uint8_t>(PowerMode::Battery)) {
    addError(errors, "power_mode", "must be one of auto|mains|battery");
  }
  if (s.data_refresh_s != 0 && (s.data_refresh_s < 30 || s.data_refresh_s > 86400)) {
    addError(errors, "data_refresh_s", "must be 0 (source schedules) or in [30, 86400]");
  }
  if (!validBatteryRefreshMin(s.battery_refresh_min)) {
    addError(errors, "battery_refresh_min", "must be one of 5|10|15|30|60");
  }
  if (!validBatteryMaxStaleMin(s.battery_max_stale_min)) {
    addError(errors, "battery_max_stale_min", "must be one of 15|30|60|120|240|480");
  } else if (validBatteryRefreshMin(s.battery_refresh_min) &&
             s.battery_max_stale_min < s.battery_refresh_min) {
    addError(errors, "battery_max_stale_min", "must be >= battery_refresh_min");
  }
  if (s.battery_interactive_timeout_s < kBatteryInteractiveMinS ||
      s.battery_interactive_timeout_s > kBatteryInteractiveMaxS) {
    addError(errors, "battery_interactive_timeout_s", "must be in [20, 600]");
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
  if (static_cast<uint8_t>(s.frontlight_hardware) >
      static_cast<uint8_t>(FrontlightHardware::Absent)) {
    addError(errors, "frontlight_hardware", "must be one of auto|present|absent");
  }
  if (s.epd_full_refresh_s != 0 &&
      (s.epd_full_refresh_s < 3600 || s.epd_full_refresh_s > 604800)) {
    addError(errors, "epd_full_refresh_s", "must be 0 (manual) or in [3600, 604800]");
  }
  if (s.idle_after_min > kIdleAfterMaxMin) {
    addError(errors, "idle_after_min", "must be 0 (disabled) or in [1, 1440]");
  }
  if (!validIdleRefreshMin(s.idle_refresh_min)) {
    addError(errors, "idle_refresh_min", "must be one of 2|5|10|15|30|60");
  }
  if (s.ble_observer_window_s < 2 || s.ble_observer_window_s > 60) {
    addError(errors, "ble_observer_window_s", "must be in [2, 60]");
  }
  if (s.ble_observer_interval_s < 30 || s.ble_observer_interval_s > 86400 ||
      s.ble_observer_interval_s < s.ble_observer_window_s) {
    addError(errors, "ble_observer_interval_s", "must be in [30, 86400] and >= scan window");
  }
  if (s.ble_observer_dedup_ms > 60000) {
    addError(errors, "ble_observer_dedup_ms", "must be in [0, 60000]");
  }
  if (s.ble_observer_rules.size() > kMaxBleObserverRules) {
    addError(errors, "ble_observer_rules", "at most 4 rules are allowed");
  }
  if (s.ble_observer_enabled && s.ble_observer_rules.empty()) {
    addError(errors, "ble_observer_rules", "at least one rule is required when scanning is enabled");
  }
  for (size_t i = 0; i < s.ble_observer_rules.size(); i++) {
    const auto& rule = s.ble_observer_rules[i];
    const std::string path = "ble_observer_rules[" + std::to_string(i) + "]";
    if (!validIdentifier(rule.id, 24)) addError(errors, path + ".id", "invalid rule identifier");
    for (size_t j = 0; j < i; j++) {
      if (s.ble_observer_rules[j].id == rule.id) addError(errors, path + ".id", "must be unique");
    }
    if (!validIdentifier(rule.event_type, 64)) {
      addError(errors, path + ".event_type", "invalid event type");
    }
    if (!validTitle(rule.title)) addError(errors, path + ".title", "must be 1-48 printable bytes");
    const bool manufacturer = rule.manufacturer_id >= 0;
    const bool service = !rule.service_uuid.empty();
    if (manufacturer == service) {
      addError(errors, path, "exactly one of manufacturer_id or service_uuid is required");
    }
    if (rule.manufacturer_id > 65535) {
      addError(errors, path + ".manufacturer_id", "must be in [0, 65535]");
    }
    if (service) {
      std::string canonical;
      if (!normalizeBleUuid(rule.service_uuid, canonical) || canonical != rule.service_uuid) {
        addError(errors, path + ".service_uuid", "must be a canonical UUID");
      }
    }
    if (rule.payload_offset > 255 || rule.payload_match.size() > kMaxBlePayloadMatchBytes ||
        static_cast<size_t>(rule.payload_offset) + rule.payload_match.size() > 256) {
      addError(errors, path + ".payload_match", "payload match exceeds its bounded range");
    }
    if (rule.fields.empty() || rule.fields.size() > kMaxBleObserverFields) {
      addError(errors, path + ".fields", "must contain 1-4 field mappings");
    }
    for (size_t j = 0; j < rule.fields.size(); j++) {
      const auto& field = rule.fields[j];
      const std::string field_path = path + ".fields[" + std::to_string(j) + "]";
      if (!validIdentifier(field.name, 24)) addError(errors, field_path + ".name", "invalid field name");
      if (field.name == "device" || field.name == "rule" || field.name == "rssi") {
        addError(errors, field_path + ".name", "reserved BLE observer metadata name");
      }
      for (size_t k = 0; k < j; k++) {
        if (rule.fields[k].name == field.name) {
          addError(errors, field_path + ".name", "must be unique within a rule");
        }
      }
      const size_t width = bleValueWidth(field.type);
      if (width == 0 || field.offset > 255 || static_cast<size_t>(field.offset) + width > 256) {
        addError(errors, field_path + ".offset", "field exceeds its bounded range");
      }
      if (!std::isfinite(field.scale) || field.scale == 0.0 || std::fabs(field.scale) > 1000000.0) {
        addError(errors, field_path + ".scale", "must be finite, nonzero and at most 1e6 magnitude");
      }
    }
    if (rule.ttl_s < 60 || rule.ttl_s > 604800) {
      addError(errors, path + ".ttl_s", "must be in [60, 604800]");
    }
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
    } else if (key == "frontlight_hardware") {
      std::string text;
      if (readString(value, key.c_str(), text, errors)) {
        FrontlightHardware mode;
        if (!frontlightHardwareFromString(text, mode)) {
          addError(errors, key, "must be one of auto|present|absent");
        } else {
          next.frontlight_hardware = mode;
        }
      }
    } else if (key == "data_refresh_s" || key == "battery_refresh_min" ||
               key == "battery_max_stale_min" || key == "battery_interactive_timeout_s" ||
               key == "sleep_start_min" ||
               key == "sleep_end_min" || key == "timezone_offset_min" ||
               key == "frontlight_brightness" || key == "frontlight_warmth" ||
               key == "epd_full_refresh_s" || key == "ble_observer_window_s" ||
               key == "ble_observer_interval_s" || key == "ble_observer_dedup_ms" ||
               key == "idle_after_min" || key == "idle_refresh_min") {
      if (!isInt(value)) {
        addError(errors, key, "must be an integer");
        continue;
      }
      const int64_t number = value.intValue();
      if ((key == "data_refresh_s" || key == "battery_refresh_min" ||
           key == "battery_max_stale_min" || key == "battery_interactive_timeout_s" ||
           key == "epd_full_refresh_s" || key == "ble_observer_window_s" ||
           key == "ble_observer_interval_s" || key == "ble_observer_dedup_ms" ||
           key == "idle_after_min" || key == "idle_refresh_min") &&
          number >= 0 && number <= UINT32_MAX) {
        if (key == "data_refresh_s") next.data_refresh_s = static_cast<uint32_t>(number);
        else if (key == "battery_refresh_min") next.battery_refresh_min = static_cast<uint32_t>(number);
        else if (key == "battery_max_stale_min") next.battery_max_stale_min = static_cast<uint32_t>(number);
        else if (key == "battery_interactive_timeout_s") next.battery_interactive_timeout_s = static_cast<uint32_t>(number);
        else if (key == "epd_full_refresh_s") next.epd_full_refresh_s = static_cast<uint32_t>(number);
        else if (key == "ble_observer_window_s") next.ble_observer_window_s = static_cast<uint32_t>(number);
        else if (key == "ble_observer_interval_s") next.ble_observer_interval_s = static_cast<uint32_t>(number);
        else if (key == "idle_after_min") next.idle_after_min = static_cast<uint32_t>(number);
        else if (key == "idle_refresh_min") next.idle_refresh_min = static_cast<uint32_t>(number);
        else next.ble_observer_dedup_ms = static_cast<uint32_t>(number);
      } else if ((key == "sleep_start_min" || key == "sleep_end_min") &&
                 number >= -1 && number <= INT32_MAX) {
        if (key == "sleep_start_min") next.sleep_start_min = static_cast<int>(number);
        else next.sleep_end_min = static_cast<int>(number);
      } else if (key == "timezone_offset_min" && number >= INT32_MIN && number <= INT32_MAX) {
        next.timezone_offset_min = static_cast<int>(number);
      } else if ((key == "frontlight_brightness" || key == "frontlight_warmth") &&
                 number >= 0 && number <= 255) {
        if (key == "frontlight_brightness") next.frontlight_brightness = static_cast<uint8_t>(number);
        else next.frontlight_warmth = static_cast<uint8_t>(number);
      } else {
        addError(errors, key, "integer is out of range");
      }
    } else if (key == "frontlight_enabled") {
      readBool(value, key.c_str(), next.frontlight_enabled, errors);
    } else if (key == "idle_critical_immediate") {
      readBool(value, key.c_str(), next.idle_critical_immediate, errors);
    } else if (key == "diagnostics_enabled") {
      readBool(value, key.c_str(), next.diagnostics_enabled, errors);
    } else if (key == "ble_observer_enabled") {
      readBool(value, key.c_str(), next.ble_observer_enabled, errors);
    } else if (key == "ble_observer_rules") {
      parseRules(value, next.ble_observer_rules, errors);
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
  if (a.power_mode != b.power_mode) changed |= kChangedPower;
  if (a.battery_refresh_min != b.battery_refresh_min ||
      a.battery_max_stale_min != b.battery_max_stale_min ||
      a.battery_interactive_timeout_s != b.battery_interactive_timeout_s) {
    changed |= kChangedBattery;
  }
  if (a.frontlight_enabled != b.frontlight_enabled ||
      a.frontlight_brightness != b.frontlight_brightness ||
      a.frontlight_warmth != b.frontlight_warmth) {
    changed |= kChangedFrontlight;
  }
  if (a.frontlight_hardware != b.frontlight_hardware) changed |= kChangedFrontlightHardware;
  if (a.data_refresh_s != b.data_refresh_s) changed |= kChangedDataRefresh;
  if (a.epd_full_refresh_s != b.epd_full_refresh_s) changed |= kChangedEpdRefresh;
  if (a.idle_after_min != b.idle_after_min || a.idle_refresh_min != b.idle_refresh_min ||
      a.idle_critical_immediate != b.idle_critical_immediate) {
    changed |= kChangedIdleRefresh;
  }
  if (a.sleep_start_min != b.sleep_start_min || a.sleep_end_min != b.sleep_end_min) {
    changed |= kChangedSleep;
  }
  if (a.timezone_offset_min != b.timezone_offset_min) changed |= kChangedTimezone;
  if (a.diagnostics_enabled != b.diagnostics_enabled) changed |= kChangedDiagnostics;
  if (a.ble_observer_enabled != b.ble_observer_enabled ||
      a.ble_observer_window_s != b.ble_observer_window_s ||
      a.ble_observer_interval_s != b.ble_observer_interval_s ||
      a.ble_observer_dedup_ms != b.ble_observer_dedup_ms ||
      a.ble_observer_rules != b.ble_observer_rules) {
    changed |= kChangedBle;
  }
  return changed;
}

bool validIdleRefreshMin(uint32_t minutes) {
  for (uint32_t choice : kIdleRefreshChoices) {
    if (choice == minutes) return true;
  }
  return false;
}

bool validBatteryRefreshMin(uint32_t minutes) {
  for (uint32_t choice : kBatteryRefreshChoices) {
    if (choice == minutes) return true;
  }
  return false;
}

bool validBatteryMaxStaleMin(uint32_t minutes) {
  for (uint32_t choice : kBatteryMaxStaleChoices) {
    if (choice == minutes) return true;
  }
  return false;
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
  "description": "Versioned non-secret device preferences. BLE is off by default; observer rules are declarative and bounded. Provisioning PoP and WiFi credentials are never settings fields.",
  "fields": {
    "language": {"type":"string","enum":["zh","en"],"default":"zh","group":"time_language"},
    "power_mode": {"type":"string","enum":["auto","mains","battery"],"default":"auto","group":"power"},
    "data_refresh_s": {"type":"int","range":[0,86400],"minimum_nonzero":30,"default":0,"group":"refresh"},
    "battery_refresh_min": {"type":"int","enum":[5,10,15,30,60],"default":15,"note":"battery mode wakes on local wall-clock multiples of this interval (doubled below 20% battery)","group":"battery"},
    "battery_max_stale_min": {"type":"int","enum":[15,30,60,120,240,480],"default":60,"note":"unchanged frames are not redrawn until this age; must be >= battery_refresh_min","group":"battery"},
    "battery_interactive_timeout_s": {"type":"int","range":[20,600],"default":60,"note":"after a button wake, return to deep sleep after this long without interaction","group":"battery"},
    "sleep_start_min": {"type":"int","range":[-1,1439],"default":-1,"note":"night pause: no wakes and no network inside the window","group":"battery"},
    "sleep_end_min": {"type":"int","range":[-1,1439],"default":-1,"group":"battery"},
    "timezone_offset_min": {"type":"int","range":[-840,840],"step":15,"default":480,"group":"time_language"},
    "frontlight_enabled": {"type":"bool","default":false,"capability":"frontlight","group":"display"},
    "frontlight_brightness": {"type":"int","range":[1,100],"default":60,"capability":"frontlight","group":"display"},
    "frontlight_warmth": {"type":"int","range":[0,100],"default":50,"capability":"frontlight","group":"display"},
    "frontlight_hardware": {"type":"string","enum":["auto","present","absent"],"default":"auto","note":"auto uses LM3630A detection; present/absent override it and gate the frontlight capability","group":"display"},
    "epd_full_refresh_s": {"type":"int","range":[0,604800],"minimum_nonzero":3600,"default":86400,"group":"refresh"},
    "idle_after_min": {"type":"int","range":[0,1440],"default":10,"note":"minutes without touch/button/web action before display updates are batched; 0 = always active","group":"display_refresh"},
    "idle_refresh_min": {"type":"int","enum":[2,5,10,15,30,60],"default":5,"note":"idle redraws happen once per interval at local wall-clock boundaries (e.g. :00/:05/:10)","group":"display_refresh"},
    "idle_critical_immediate": {"type":"bool","default":true,"note":"a newly active critical alert redraws immediately even when idle","group":"display_refresh"},
    "diagnostics_enabled": {"type":"bool","default":true,"group":"diagnostics"},
    "ble_observer_enabled": {"type":"bool","default":false,"capability":"ble_observer","group":"ble"},
    "ble_observer_window_s": {"type":"int","range":[2,60],"default":10,"group":"ble"},
    "ble_observer_interval_s": {"type":"int","range":[30,86400],"default":300,"note":"delay after the previous scan window; battery scans only once per wake window","group":"ble"},
    "ble_observer_dedup_ms": {"type":"int","range":[0,60000],"default":5000,"group":"ble"},
    "ble_observer_rules": {"type":"array","default":[],"max_items":4,"required_when":"ble_observer_enabled","group":"ble","item":{
      "required":["id","fields"],"id":{"type":"string","max_bytes":24},"event_type":{"type":"string","default":"ble.sensor","max_bytes":64},"title":{"type":"string","default":"BLE sensor","max_bytes":48},
      "match":{"one_of":[{"manufacturer_id":{"type":"int","range":[0,65535]}},{"service_uuid":{"type":"string","format":"16/32/128-bit UUID"}}]},
      "payload_offset":{"type":"int","range":[0,255],"default":0,"note":"relative to payload after company ID or service UUID"},"payload_match":{"type":"hex","max_bytes":12,"default":""},
      "fields":{"type":"array","min_items":1,"max_items":4,"item":{"name":{"type":"string","max_bytes":24,"forbidden_values":["device","rule","rssi"]},"offset":{"type":"int","range":[0,255]},"type":{"enum":["u8","i8","u16le","i16le","u32le","i32le","f32le"]},"scale":{"type":"number","default":1,"nonzero":true,"max_abs":1000000}}},
      "ttl_s":{"type":"int","range":[60,604800],"default":86400}
    }}
  },
  "ble": {"roles":["wifi_prov","ble_scan"],"runtime_default":"off","provisioning_trigger":"local_physical_only","security":"wifi_prov_security_1_with_per_window_pop","deep_sleep_realtime":false,"scripting":false},
  "deep_sleep": {"http_reachable":false,"ble_reachable":false,"note":"Recovery AP, BLE provisioning and observer wake windows pause automatic sleep; BLE is stopped before deep sleep."}
})JSON";
}

}  // namespace eego::settings
