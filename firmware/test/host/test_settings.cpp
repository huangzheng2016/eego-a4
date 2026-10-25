// SPDX-License-Identifier: GPL-3.0-or-later
// device_settings settings_model: JSON round trip, strict patches, validation.
#include <string>

#include "device_settings/settings_model.h"
#include "test_framework.h"

using namespace eego::settings;
using alert_core::JsonValue;

namespace {

JsonValue json(const std::string& text) {
  JsonValue v;
  alert_core::JsonParseError err;
  CHECK_MSG(alert_core::jsonParse(text, v, err), text << ": " << err.message);
  return v;
}

bool patchOk(const Settings& base, const std::string& patch, Settings& out, FieldErrors& errors) {
  errors.clear();
  return mergePatch(base, json(patch), out, errors);
}

bool hasField(const FieldErrors& errors, const std::string& field) {
  for (const auto& e : errors)
    if (e.field == field) return true;
  return false;
}

std::string describe(const FieldErrors& errors) {
  std::string s;
  for (const auto& e : errors) s += e.field + ": " + e.message + "; ";
  return s;
}

}  // namespace

TEST(settings, defaults_are_valid) {
  const Settings s;
  FieldErrors errors;
  validate(s, errors);
  CHECK_MSG(errors.empty(), describe(errors));
  CHECK(s.frontlight_hardware == FrontlightHardware::Auto);
  CHECK_EQ(s.timezone_offset_min, 480);  // UTC+8 default
}

TEST(settings, frontlight_hardware_names) {
  for (auto mode : {FrontlightHardware::Auto, FrontlightHardware::Present, FrontlightHardware::Absent}) {
    FrontlightHardware back = FrontlightHardware::Auto;
    CHECK(frontlightHardwareFromString(frontlightHardwareName(mode), back));
    CHECK(back == mode);
  }
  CHECK_EQ(std::string(frontlightHardwareName(FrontlightHardware::Present)), std::string("present"));
  FrontlightHardware out;
  CHECK(!frontlightHardwareFromString("Present", out));
  CHECK(!frontlightHardwareFromString("", out));
  CHECK(!frontlightHardwareFromString("yes", out));
}

TEST(settings, json_round_trip) {
  Settings s;
  s.language = "en";
  s.power_mode = PowerMode::Battery;
  s.battery_poll_s = 600;
  s.data_refresh_s = 120;
  s.idle_sleep_s = 0;
  s.sleep_start_min = 23 * 60;
  s.sleep_end_min = 7 * 60;
  s.timezone_offset_min = -330;
  s.frontlight_enabled = true;
  s.frontlight_brightness = 33;
  s.frontlight_warmth = 80;
  s.frontlight_hardware = FrontlightHardware::Absent;
  s.epd_full_refresh_s = 7200;
  s.diagnostics_enabled = false;
  s.ble_observer_enabled = true;
  BleObserverRule rule;
  rule.id = "temp1";
  rule.manufacturer_id = 0x0499;
  rule.payload_match = {0x99, 0x04};
  rule.fields.push_back({"temperature", 2, BleValueType::Int16Le, 0.01});
  s.ble_observer_rules.push_back(rule);
  FieldErrors errors;
  validate(s, errors);
  REQUIRE(errors.empty() || (std::fprintf(stderr, "%s\n", describe(errors).c_str()), false));

  const JsonValue encoded = toJson(s);
  const JsonValue* hw = encoded.find("frontlight_hardware");
  REQUIRE(hw != nullptr);
  CHECK_EQ(hw->stringValue(), std::string("absent"));

  // Re-parse the serialized text and apply it as a full patch on defaults.
  Settings decoded;
  CHECK_MSG(patchOk(Settings{}, encoded.toString(), decoded, errors), describe(errors));
  CHECK_EQ(toJson(decoded).toString(), encoded.toString());
  CHECK(decoded.frontlight_hardware == FrontlightHardware::Absent);
  CHECK(decoded.ble_observer_rules.size() == 1 && decoded.ble_observer_rules[0] == rule);
  CHECK_EQ(changedFields(s, decoded), 0u);
}

TEST(settings, patch_frontlight_hardware) {
  const Settings base;
  Settings out;
  FieldErrors errors;
  CHECK(patchOk(base, R"({"frontlight_hardware":"present"})", out, errors));
  CHECK(out.frontlight_hardware == FrontlightHardware::Present);
  CHECK(changedFields(base, out) == kChangedFrontlightHardware);
  CHECK(patchOk(out, R"({"frontlight_hardware":"auto"})", out, errors));
  CHECK(out.frontlight_hardware == FrontlightHardware::Auto);

  out = base;
  out.frontlight_warmth = 11;
  const Settings before = out;
  for (const char* bad : {R"({"frontlight_hardware":"maybe"})", R"({"frontlight_hardware":1})",
                          R"({"frontlight_hardware":null})", R"({"frontlight_hardware":"AUTO"})"}) {
    Settings target = before;
    CHECK_MSG(!patchOk(before, bad, target, errors), bad);
    CHECK_MSG(hasField(errors, "frontlight_hardware"), bad << " -> " << describe(errors));
    CHECK_EQ(toJson(target).toString(), toJson(before).toString());  // out unchanged on failure
  }
  // Validation rejects an out-of-range enum value even if set programmatically.
  Settings forged;
  forged.frontlight_hardware = static_cast<FrontlightHardware>(7);
  errors.clear();
  validate(forged, errors);
  CHECK(hasField(errors, "frontlight_hardware"));
}

TEST(settings, patch_rejects_bad_values) {
  const Settings base;
  Settings out;
  FieldErrors errors;
  const struct {
    const char* patch;
    const char* field;
  } cases[] = {
      {R"({"language":"fr"})", "language"},
      {R"({"power_mode":"solar"})", "power_mode"},
      {R"({"battery_poll_s":10})", "battery_poll_s"},
      {R"({"battery_poll_s":"900"})", "battery_poll_s"},
      {R"({"data_refresh_s":5})", "data_refresh_s"},
      {R"({"idle_sleep_s":5})", "idle_sleep_s"},
      {R"({"timezone_offset_min":481})", "timezone_offset_min"},
      {R"({"timezone_offset_min":900})", "timezone_offset_min"},
      {R"({"frontlight_brightness":0})", "frontlight_brightness"},
      {R"({"frontlight_brightness":101})", "frontlight_brightness"},
      {R"({"frontlight_warmth":-1})", "frontlight_warmth"},
      {R"({"frontlight_enabled":"yes"})", "frontlight_enabled"},
      {R"({"epd_full_refresh_s":60})", "epd_full_refresh_s"},
      {R"({"sleep_start_min":60})", "sleep_start_min"},  // end still -1
      {R"({"sleep_start_min":60,"sleep_end_min":60})", "sleep_end_min"},
      {R"({"schema_version":2})", "schema_version"},
      {R"({"unknown_key":1})", "unknown_key"},
  };
  for (const auto& c : cases) {
    out = base;
    const bool ok = patchOk(base, c.patch, out, errors);
    CHECK_MSG(!ok, c.patch);
    CHECK_MSG(hasField(errors, c.field), c.patch << " -> " << describe(errors));
  }
  CHECK(!patchOk(base, "[1]", out, errors));
}

TEST(settings, patch_accepts_good_values) {
  const Settings base;
  Settings out;
  FieldErrors errors;
  CHECK_MSG(patchOk(base, R"({"schema_version":1,"language":"en","timezone_offset_min":-210,
      "sleep_start_min":1380,"sleep_end_min":420,"idle_sleep_s":0,"data_refresh_s":30})",
                    out, errors),
            describe(errors));
  CHECK_EQ(out.timezone_offset_min, -210);
  const uint32_t changed = changedFields(base, out);
  CHECK(changed & kChangedLanguage);
  CHECK(changed & kChangedTimezone);
  CHECK(changed & kChangedSleep);
  CHECK(changed & kChangedDataRefresh);
  CHECK(!(changed & kChangedFrontlight));
}

TEST(settings, legacy_display_mapping) {
  Settings s;
  applyLegacyDisplay(json(R"({"brightness":0,"warmth":20})"), s);
  CHECK(!s.frontlight_enabled);
  CHECK_EQ(s.frontlight_warmth, uint8_t{20});
  applyLegacyDisplay(json(R"({"brightness":45})"), s);
  CHECK(s.frontlight_enabled);
  CHECK_EQ(s.frontlight_brightness, uint8_t{45});
  FieldErrors errors;
  validateLegacyDisplayCapability(json(R"({"brightness":10})"), s, false, errors);
  CHECK(hasField(errors, "admin.display.brightness"));
  errors.clear();
  validateLegacyDisplayCapability(json(R"({"brightness":10})"), s, true, errors);
  CHECK(errors.empty());
}

TEST(settings, scheduled_sleep_window) {
  Settings s;
  s.timezone_offset_min = 0;
  uint64_t wake = 0;
  uint32_t remaining = 0;
  const uint64_t midnight = 1767225600ULL;  // 2026-01-01T00:00Z
  CHECK(!scheduledSleep(s, midnight, wake, remaining));  // disabled
  s.sleep_start_min = 23 * 60;
  s.sleep_end_min = 7 * 60;  // overnight
  CHECK(scheduledSleep(s, midnight + 2 * 3600, wake, remaining));
  CHECK_EQ(wake, midnight + 7 * 3600);
  CHECK_EQ(remaining, 5u * 3600u);
  CHECK(!scheduledSleep(s, midnight + 12 * 3600, wake, remaining));
  s.timezone_offset_min = 480;  // 02:00Z = 10:00 local -> awake
  CHECK(!scheduledSleep(s, midnight + 2 * 3600, wake, remaining));
}

TEST(settings, schema_mentions_frontlight_hardware) {
  const std::string schema = schemaJson();
  CHECK(schema.find("frontlight_hardware") != std::string::npos);
  JsonValue parsed;
  alert_core::JsonParseError err;
  CHECK_MSG(alert_core::jsonParse(schema, parsed, err), err.message);
}
