// SPDX-License-Identifier: GPL-3.0-or-later
#include "admin/admin_config.h"
#include "alert_core/json_writer.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#include "admin/admin_log.h"
#include "atomic_file.h"
#include "alert_core/types.h"
#include "device_settings/device_settings.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "net_stack/credentials.h"
#include "net_stack/wifi_list.h"

namespace eego::admin {

namespace {

constexpr size_t kMaxConfigBodyBytes = 4 * 1024;
constexpr size_t kMaxOverrides = 64;
constexpr size_t kMaxWifiList = net::kMaxWifiNetworks;
constexpr size_t kMaxMuteRules = 32;
constexpr int64_t kSchemaVersion = 1;
constexpr int64_t kMaxMuteExpiresAt = 4102444800LL;

const alert_core::JsonValue* objGet(const alert_core::JsonValue& obj, const char* key) {
  return obj.find(key);
}

alert_core::JsonValue::Object& mutableItems(alert_core::JsonValue& obj) {
  return const_cast<alert_core::JsonValue::Object&>(obj.objectItems());
}

void objSet(alert_core::JsonValue& obj, const std::string& key, alert_core::JsonValue val) {
  for (auto& kv : mutableItems(obj)) {
    if (kv.first == key) {
      kv.second = std::move(val);
      return;
    }
  }
  mutableItems(obj).emplace_back(key, std::move(val));
}

alert_core::JsonValue cloneDom(const alert_core::JsonValue& v) {
  using alert_core::JsonValue;
  switch (v.type()) {
    case JsonValue::Type::Null: return JsonValue::makeNull();
    case JsonValue::Type::Bool: return JsonValue::makeBool(v.boolValue());
    case JsonValue::Type::Int: return JsonValue::makeInt(v.intValue());
    case JsonValue::Type::Double: return JsonValue::makeDouble(v.doubleValue());
    case JsonValue::Type::String: return JsonValue::makeString(v.stringValue());
    case JsonValue::Type::Array: {
      JsonValue::Array out;
      out.reserve(v.arrayItems().size());
      for (const auto& item : v.arrayItems()) out.push_back(cloneDom(item));
      return JsonValue::makeArray(std::move(out));
    }
    case JsonValue::Type::Object: {
      JsonValue::Object out;
      out.reserve(v.objectItems().size());
      for (const auto& kv : v.objectItems()) {
        out.emplace_back(kv.first, cloneDom(kv.second));
      }
      return JsonValue::makeObject(std::move(out));
    }
  }
  return JsonValue::makeNull();
}

bool keyEndsWith(const std::string& s, const char* suffix) {
  const size_t n = strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

bool isSensitiveKey(const std::string& key_lower) {
  if (keyEndsWith(key_lower, "_ref")) return false;
  if (key_lower.find("password") != std::string::npos ||
      key_lower.find("passwd") != std::string::npos ||
      key_lower.find("passphrase") != std::string::npos ||
      key_lower.find("secret") != std::string::npos ||
      key_lower.find("token") != std::string::npos) {
    return true;
  }
  if (key_lower == "key" || key_lower == "apikey" || key_lower == "api-key") return true;
  if (keyEndsWith(key_lower, "_key") && key_lower != "dedup_key") return true;
  return false;
}

bool isSensitiveHeaderName(const std::string& name_lower) {
  return net::CredentialStore::sensitiveHeaderName(name_lower);
}

alert_core::JsonValue maskDom(const alert_core::JsonValue& v, const std::string& key_ctx) {
  using alert_core::JsonValue;
  switch (v.type()) {
    case JsonValue::Type::Array: {
      JsonValue::Array out;
      out.reserve(v.arrayItems().size());
      for (const auto& item : v.arrayItems()) out.push_back(maskDom(item, key_ctx));
      return JsonValue::makeArray(std::move(out));
    }
    case JsonValue::Type::Object: {
      JsonValue::Object out;
      out.reserve(v.objectItems().size());
      for (const auto& kv : v.objectItems()) {
        std::string lower(kv.first.size(), '\0');
        for (size_t i = 0; i < kv.first.size(); i++) {
          lower[i] = static_cast<char>(tolower(static_cast<unsigned char>(kv.first[i])));
        }
        if (lower == "header_secret_refs" && kv.second.type() == JsonValue::Type::Object) {
          out.emplace_back(kv.first, cloneDom(kv.second));
        } else if (lower == "headers" && kv.second.type() == JsonValue::Type::Object) {
          JsonValue::Object hdrs;
          hdrs.reserve(kv.second.objectItems().size());
          for (const auto& h : kv.second.objectItems()) {
            std::string hl(h.first.size(), '\0');
            for (size_t i = 0; i < h.first.size(); i++) {
              hl[i] = static_cast<char>(tolower(static_cast<unsigned char>(h.first[i])));
            }
            if (h.second.type() == JsonValue::Type::String && isSensitiveHeaderName(hl)) {
              hdrs.emplace_back(h.first, JsonValue::makeString("********"));
            } else {
              hdrs.emplace_back(h.first, maskDom(h.second, h.first));
            }
          }
          out.emplace_back(kv.first, JsonValue::makeObject(std::move(hdrs)));
        } else if (kv.second.type() == JsonValue::Type::String && isSensitiveKey(lower)) {
          out.emplace_back(kv.first, JsonValue::makeString("********"));
        } else {
          out.emplace_back(kv.first, maskDom(kv.second, kv.first));
        }
      }
      return JsonValue::makeObject(std::move(out));
    }
    default:
      return cloneDom(v);
  }
}


bool isControlFree(const std::string& s) {
  for (unsigned char c : s) {
    if (c < 0x20 || c == 0x7F) return false;
  }
  return true;
}

bool validSsid(const std::string& s) {
  return !s.empty() && s.size() <= 32 && isControlFree(s);
}

bool validPassRef(const std::string& s) {
  if (s.empty()) return true;
  if (s.compare(0, 9, "nvs:wifi/") != 0) return false;
  const std::string idx = s.substr(9);
  if (idx.empty() || idx.size() > 2) return false;
  for (char c : idx) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

bool validFontPath(const std::string& s) {
  if (s.empty()) return true;
  if (s.size() > 128 || s[0] != '/') return false;
  if (s.find("..") != std::string::npos) return false;
  return isControlFree(s);
}

void addErr(FieldErrors& errs, const std::string& field, const std::string& msg) {
  errs.push_back({field, msg});
}

void validateAdminSection(const alert_core::JsonValue& admin, FieldErrors& errs,
                          const std::string& current_wifi_list_json, bool strict_unknown) {
  using alert_core::JsonValue;
  if (admin.type() != JsonValue::Type::Object) {
    addErr(errs, "admin", "must be an object");
    return;
  }
  for (const auto& kv : admin.objectItems()) {
    const std::string& k = kv.first;
    const JsonValue& v = kv.second;
    if (k == "wifi_list") {
      if (v.type() != JsonValue::Type::Array) {
        addErr(errs, "admin.wifi_list", "must be an array");
        continue;
      }
      if (v.toString() != current_wifi_list_json) {
        addErr(errs, "admin.wifi_list",
               "read-only here; manage via /api/v1/config/wifi");
        continue;
      }
      if (v.arrayItems().size() > kMaxWifiList) {
        addErr(errs, "admin.wifi_list",
               "too many entries (max " + std::to_string(kMaxWifiList) + ")");
      }
    } else if (k == "network") {
      if (v.type() != JsonValue::Type::Object) {
        addErr(errs, "admin.network", "must be an object");
        continue;
      }
      for (const auto& nkv : v.objectItems()) {
        if (nkv.first == "auto_ap_fallback") {
          if (nkv.second.type() != JsonValue::Type::Bool) {
            addErr(errs, "admin.network.auto_ap_fallback", "must be a boolean");
          }
        } else {
          if (strict_unknown) addErr(errs, "admin.network." + nkv.first, "unknown field");
        }
      }
    } else if (k == "security") {
      if (v.type() != JsonValue::Type::Object) {
        addErr(errs, "admin.security", "must be an object");
        continue;
      }
      for (const auto& nkv : v.objectItems()) {
        if (nkv.first == "session_ttl_s") {
          if (nkv.second.type() != JsonValue::Type::Int ||
              nkv.second.intValue() < 300 || nkv.second.intValue() > 86400) {
            addErr(errs, "admin.security.session_ttl_s", "must be an integer in [300, 86400]");
          }
        } else if (nkv.first == "login_lock_threshold") {
          if (nkv.second.type() != JsonValue::Type::Int ||
              nkv.second.intValue() < 3 || nkv.second.intValue() > 10) {
            addErr(errs, "admin.security.login_lock_threshold",
                   "must be an integer in [3, 10]");
          }
        } else {
          if (strict_unknown) addErr(errs, "admin.security." + nkv.first, "unknown field");
        }
      }
    } else if (k == "display") {
      if (v.type() != JsonValue::Type::Object) {
        addErr(errs, "admin.display", "must be an object");
        continue;
      }
      for (const auto& nkv : v.objectItems()) {
        if (nkv.first == "font") {
          if (nkv.second.type() != JsonValue::Type::String ||
              !validFontPath(nkv.second.stringValue())) {
            addErr(errs, "admin.display.font",
                   "must be an absolute path without '..', or empty for built-in");
          }
        } else {
          if (strict_unknown) addErr(errs, "admin.display." + nkv.first, "unknown field");
        }
      }
    } else if (k == "logging") {
      if (v.type() != JsonValue::Type::Object) {
        addErr(errs, "admin.logging", "must be an object");
        continue;
      }
      for (const auto& nkv : v.objectItems()) {
        if (nkv.first == "level") {
          LogLevel lv;
          if (nkv.second.type() != JsonValue::Type::String ||
              !logLevelFromString(nkv.second.stringValue(), lv)) {
            addErr(errs, "admin.logging.level", "must be one of ERROR|WARN|INFO|DEBUG");
          }
        } else if (nkv.first == "sd_log") {
          if (nkv.second.type() != JsonValue::Type::Bool) {
            addErr(errs, "admin.logging.sd_log", "must be a boolean");
          }
        } else {
          if (strict_unknown) addErr(errs, "admin.logging." + nkv.first, "unknown field");
        }
      }
    } else {
      if (strict_unknown) addErr(errs, "admin." + k, "unknown field");
    }
  }
}

void validateOverridesSection(const alert_core::JsonValue& obj, FieldErrors& errs) {
  using alert_core::JsonValue;
  if (obj.type() != JsonValue::Type::Object) {
    addErr(errs, "alert_type_overrides", "must be an object");
    return;
  }
  if (obj.objectItems().size() > kMaxOverrides) {
    addErr(errs, "alert_type_overrides", "too many entries (max 64)");
    return;
  }
  for (const auto& kv : obj.objectItems()) {
    const std::string base = "alert_type_overrides." + kv.first;
    if (kv.first.empty() || kv.first.size() > 64 || !isControlFree(kv.first)) {
      addErr(errs, base, "invalid type id");
      continue;
    }
    if (kv.second.type() != JsonValue::Type::Object) {
      addErr(errs, base, "must be an object");
      continue;
    }
    if (kv.second.objectItems().empty()) {
      addErr(errs, base, "empty override");
      continue;
    }
    for (const auto& okv : kv.second.objectItems()) {
      const std::string f = base + "." + okv.first;
      if (okv.first == "severity") {
        alert_core::Severity sev;
        if (okv.second.type() != JsonValue::Type::String ||
            !alert_core::severityFromString(okv.second.stringValue(), sev)) {
          addErr(errs, f, "must be one of critical|warning|info");
        }
      } else if (okv.first == "ttl_s") {
        if (okv.second.type() != JsonValue::Type::Int || okv.second.intValue() < 0 ||
            okv.second.intValue() > alert_core::kMaxTtlSeconds) {
          addErr(errs, f, "must be an integer in [0, 31622400]");
        }
      } else if (okv.first == "name_zh" || okv.first == "name_en") {
        if (okv.second.type() != JsonValue::Type::String ||
            okv.second.stringValue().size() > 64) {
          addErr(errs, f, "must be a string of at most 64 bytes");
        }
      } else if (okv.first == "muted") {
        if (okv.second.type() != JsonValue::Type::Bool) {
          addErr(errs, f, "must be a boolean");
        }
      } else {
        addErr(errs, f, "unknown field");
      }
    }
  }
}


bool isLowerHexNormalized(std::string& s, size_t len) {
  if (s.size() != len) return false;
  for (char& c : s) {
    if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

bool muteRuleFromJson(const alert_core::JsonValue& v, alert_core::MuteRule& out,
                      std::string& err_field, std::string& err_msg) {
  using alert_core::JsonValue;
  if (v.type() != JsonValue::Type::Object) {
    err_field.clear();
    err_msg = "must be an object";
    return false;
  }
  const JsonValue* mb = objGet(v, "match_by");
  if (mb == nullptr || mb->type() != JsonValue::Type::String) {
    err_field = "match_by";
    err_msg = "must be one of type|source|fingerprint";
    return false;
  }
  const std::string& mbs = mb->stringValue();
  if (mbs == "type") {
    out.match_by = alert_core::MuteRule::MatchBy::Type;
  } else if (mbs == "source") {
    out.match_by = alert_core::MuteRule::MatchBy::Source;
  } else if (mbs == "fingerprint") {
    out.match_by = alert_core::MuteRule::MatchBy::Fingerprint;
  } else {
    err_field = "match_by";
    err_msg = "must be one of type|source|fingerprint";
    return false;
  }
  for (const auto& kv : v.objectItems()) {
    const JsonValue& val = kv.second;
    if (kv.first == "match_by") {
      continue;
    } else if (kv.first == "value") {
      if (val.type() != JsonValue::Type::String) {
        err_field = "value";
        err_msg = "must be a string";
        return false;
      }
      std::string s = val.stringValue();
      if (out.match_by == alert_core::MuteRule::MatchBy::Fingerprint) {
        if (!isLowerHexNormalized(s, alert_core::kFingerprintHexLen)) {
          err_field = "value";
          err_msg = "fingerprint rule value must be 40 hex chars";
          return false;
        }
      } else if (s.empty() || s.size() > alert_core::kMaxTypeIdLen || !isControlFree(s)) {
        err_field = "value";
        err_msg = "must be 1..64 control-free chars";
        return false;
      }
      out.value = std::move(s);
    } else if (kv.first == "expires_at") {
      if (val.type() != JsonValue::Type::Int || val.intValue() < 0 ||
          val.intValue() > kMaxMuteExpiresAt) {
        err_field = "expires_at";
        err_msg = "must be an integer in [0, 4102444800] (absolute unix seconds)";
        return false;
      }
      out.expires_at = static_cast<uint64_t>(val.intValue());
    } else {
      err_field = kv.first;
      err_msg = "unknown field";
      return false;
    }
  }
  if (out.value.empty()) {
    err_field = "value";
    err_msg = "match_by and value are required";
    return false;
  }
  return true;
}

alert_core::JsonValue muteRuleToJson(const alert_core::MuteRule& r) {
  using alert_core::JsonValue;
  const char* by = "type";
  switch (r.match_by) {
    case alert_core::MuteRule::MatchBy::Source: by = "source"; break;
    case alert_core::MuteRule::MatchBy::Fingerprint: by = "fingerprint"; break;
    default: break;
  }
  return ::eego::json::ObjectWriter(3)
      .string("match_by", by)
      .string("value", r.value)
      .integer("expires_at", static_cast<int64_t>(r.expires_at))
      .take();
}

void validateMuteRulesSection(const alert_core::JsonValue& arr, FieldErrors& errs) {
  using alert_core::JsonValue;
  if (arr.type() != JsonValue::Type::Array) {
    addErr(errs, "mute_rules", "must be an array");
    return;
  }
  if (arr.arrayItems().size() > kMaxMuteRules) {
    addErr(errs, "mute_rules", "too many rules (max 32)");
    return;
  }
  for (size_t i = 0; i < arr.arrayItems().size(); i++) {
    alert_core::MuteRule tmp;
    std::string f, m;
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "mute_rules[%zu]", i);
    if (!muteRuleFromJson(arr.arrayItems()[i], tmp, f, m)) {
      addErr(errs, f.empty() ? std::string(prefix) : std::string(prefix) + "." + f, m);
    }
  }
}

AdminDomainConfig parseAdminView(const alert_core::JsonValue& dom) {
  using alert_core::JsonValue;
  AdminDomainConfig out;
  const JsonValue* admin = objGet(dom, "admin");
  if (admin == nullptr || admin->type() != JsonValue::Type::Object) return out;
  const JsonValue* v = objGet(*admin, "network");
  if (v && v->type() == JsonValue::Type::Object) {
    if (const JsonValue* b = objGet(*v, "auto_ap_fallback")) {
      out.auto_ap_fallback = b->boolValue(out.auto_ap_fallback);
    }
  }
  v = objGet(*admin, "security");
  if (v && v->type() == JsonValue::Type::Object) {
    if (const JsonValue* b = objGet(*v, "session_ttl_s")) {
      const int64_t t = b->intValue(static_cast<int64_t>(out.session_ttl_s));
      if (t >= 300 && t <= 86400) out.session_ttl_s = static_cast<uint32_t>(t);
    }
    if (const JsonValue* b = objGet(*v, "login_lock_threshold")) {
      const int64_t t = b->intValue(static_cast<int64_t>(out.login_lock_threshold));
      if (t >= 3 && t <= 10) out.login_lock_threshold = static_cast<uint32_t>(t);
    }
  }
  v = objGet(*admin, "display");
  if (v && v->type() == JsonValue::Type::Object) {
    if (const JsonValue* b = objGet(*v, "font")) {
      if (b->type() == JsonValue::Type::String && validFontPath(b->stringValue())) {
        out.font = b->stringValue();
      }
    }
  }
  v = objGet(*admin, "logging");
  if (v && v->type() == JsonValue::Type::Object) {
    if (const JsonValue* b = objGet(*v, "level")) {
      LogLevel lv;
      if (b->type() == JsonValue::Type::String && logLevelFromString(b->stringValue(), lv)) {
        out.log_level = b->stringValue();
      }
    }
    if (const JsonValue* b = objGet(*v, "sd_log")) {
      out.sd_log = b->boolValue(out.sd_log);
    }
  }
  return out;
}

alert_core::JsonValue makeDefaultDom() {
  using alert_core::JsonValue;
  JsonValue admin = ::eego::json::ObjectWriter(5)
      .value("wifi_list", ::eego::json::JsonValue(JsonValue::makeArray({})))
      .value("network", ::eego::json::JsonValue(::eego::json::ObjectWriter(1).boolean("auto_ap_fallback", false).take()))
      .value("security", ::eego::json::JsonValue(::eego::json::ObjectWriter(2).integer("session_ttl_s", 86400).integer("login_lock_threshold", 5).take()))
      .value("display", ::eego::json::JsonValue(::eego::json::ObjectWriter(1).string("font", "").take()))
      .value("logging", ::eego::json::JsonValue(::eego::json::ObjectWriter(2).string("level", "INFO").boolean("sd_log", false).take()))
      .take();
  return ::eego::json::ObjectWriter(4)
      .integer("schema_version", kSchemaVersion)
      .value("admin", ::eego::json::JsonValue(std::move(admin)))
      .value("alert_type_overrides", ::eego::json::JsonValue(::eego::json::ObjectWriter(0).take()))
      .value("mute_rules", ::eego::json::JsonValue(JsonValue::makeArray({})))
      .take();
}

bool validateLoadedDom(const alert_core::JsonValue& dom, FieldErrors& errs) {
  using alert_core::JsonValue;
  if (dom.type() != JsonValue::Type::Object) {
    addErr(errs, "", "config root must be an object");
    return false;
  }
  const JsonValue* sv = objGet(dom, "schema_version");
  if (sv == nullptr || sv->type() != JsonValue::Type::Int || sv->intValue() != kSchemaVersion) {
    addErr(errs, "schema_version", "unsupported schema_version (expected 1)");
    return false;
  }
  const JsonValue* admin = objGet(dom, "admin");
  if (admin == nullptr || admin->type() != JsonValue::Type::Object) {
    addErr(errs, "admin", "missing or not an object");
    return false;
  }
  if (const JsonValue* wl = objGet(*admin, "wifi_list")) {
    if (wl->type() != JsonValue::Type::Array || wl->arrayItems().size() > kMaxWifiList) {
      addErr(errs, "admin.wifi_list", "invalid wifi_list");
      return false;
    }
    for (size_t i = 0; i < wl->arrayItems().size(); i++) {
      const JsonValue& e = wl->arrayItems()[i];
      const JsonValue* ssid = e.find("ssid");
      const JsonValue* ref = e.find("pass_ref");
      if (e.type() != JsonValue::Type::Object || ssid == nullptr ||
          ssid->type() != JsonValue::Type::String || !validSsid(ssid->stringValue()) ||
          ref == nullptr || ref->type() != JsonValue::Type::String ||
          !validPassRef(ref->stringValue())) {
        addErr(errs, "admin.wifi_list", "invalid entry");
        return false;
      }
    }
  }
  {
    FieldErrors sub;
    validateAdminSection(*admin, sub, objGet(*admin, "wifi_list")
                                          ? objGet(*admin, "wifi_list")->toString()
                                          : "[]", false);
    for (auto& e : sub) {
      if (e.field == "admin.wifi_list") continue;
      errs.push_back(e);
    }
  }
  if (const JsonValue* ov = objGet(dom, "alert_type_overrides")) {
    const size_t before = errs.size();
    validateOverridesSection(*ov, errs);
    if (errs.size() != before) return false;
  }
  if (const JsonValue* mr = objGet(dom, "mute_rules")) {
    const size_t before = errs.size();
    validateMuteRulesSection(*mr, errs);
    if (errs.size() != before) return false;
  }
  return errs.empty();
}

// Top-level keys written by older firmware that must not linger in config.json.
// `sources` (pre-dashboard data sources) could carry plaintext HTTP headers or
// tokens; data sources now live in the dashboard bundle with secrets in NVS.
bool dropLegacyTopLevelKeys(alert_core::JsonValue& dom) {
  if (dom.type() != alert_core::JsonValue::Type::Object) return false;
  auto& items = mutableItems(dom);
  const size_t before = items.size();
  items.erase(std::remove_if(items.begin(), items.end(),
                             [](const auto& kv) { return kv.first == "sources"; }),
              items.end());
  return items.size() != before;
}

bool atomicConfigWrite(const std::string& path, const std::string& text, bool& replaced) {
  replaced = false;
  const size_t slash = path.find_last_of('/');
  if (slash != std::string::npos && slash > 0) {
    const std::string dir = path.substr(0, slash);
    if (mkdir(dir.c_str(), 0775) != 0 && errno != EEXIST) {
      // fopen below reports the authoritative error without exposing contents.
    }
  }
  // FatFS-safe replace (tmp + fsync, target -> .bak, tmp -> target, drop .bak).
  bool ok = fsutil::atomicWriteFile(path, text);
  replaced = ok;
  if (ok) {
    std::string back;
    if (FILE* f = fopen(path.c_str(), "rb")) {
      char buffer[512];
      size_t count = 0;
      while ((count = fread(buffer, 1, sizeof(buffer), f)) > 0) back.append(buffer, count);
      if (ferror(f)) ok = false;
      if (fclose(f) != 0) ok = false;
    } else {
      ok = false;
    }
    ok = ok && back == text;
  }
  return ok;
}

}  // namespace

// ===========================================================================
// ConfigStore
// ===========================================================================

ConfigStore& ConfigStore::instance() {
  static ConfigStore inst;
  return inst;
}

esp_err_t ConfigStore::init(const std::string& path) {
  if (mutex_ == nullptr) mutex_ = xSemaphoreCreateMutex();
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  path_ = path;
  loaded_ = true;

  dom_ = makeDefaultDom();
  admin_ = AdminDomainConfig{};
  valid_ = true;

  if (fsutil::recoverFile(path_)) ADMIN_LOG_WARN("config.json restored from interrupted replace");
  std::string text;
  bool read_ok = false;
  if (FILE* f = fopen(path_.c_str(), "rb")) {
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    read_ok = !ferror(f);
    fclose(f);
  }

  if (read_ok && !text.empty()) {
    alert_core::JsonValue parsed;
    alert_core::JsonParseError perr;
    FieldErrors errs;
    const bool parse_ok = alert_core::jsonParse(text, parsed, perr);
    net::CredentialStore::secureErase(text);
    if (parse_ok) {
      validateLoadedDom(parsed, errs);
      if (errs.empty()) {
        dom_ = std::move(parsed);
        admin_ = parseAdminView(dom_);
        // One-time cleanup: drop the legacy `sources` section (possible
        // plaintext headers) and rewrite the file atomically so it is gone on
        // disk too; later boots find nothing to drop, so this logs once.
        if (dropLegacyTopLevelKeys(dom_)) {
          std::string cleaned = dom_.toString();
          bool replaced = false;
          if (atomicConfigWrite(path_, cleaned, replaced)) {
            ADMIN_LOG_WARN("config.json: removed legacy top-level 'sources' section");
          } else {
            ADMIN_LOG_ERROR("config.json: legacy 'sources' dropped in RAM; rewrite failed");
          }
          net::CredentialStore::secureErase(cleaned);
        }
      } else {
        valid_ = false;
      }
    } else {
      valid_ = false;
    }
    if (!valid_) {
      const std::string bak = path_ + ".corrupt";
      remove(bak.c_str());
      if (rename(path_.c_str(), bak.c_str()) != 0) {
        ADMIN_LOG_ERROR("config corrupt; backup rename failed (%s)", path_.c_str());
      } else {
        ADMIN_LOG_ERROR("config corrupt; backed up to config.json.corrupt, loading defaults");
      }
    }
  } else if (!read_ok) {
    if (FILE* f = fopen(path_.c_str(), "rb")) {
      fclose(f);
      valid_ = false;
    }
  }

  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  return ESP_OK;
}

bool ConfigStore::valid() const {
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  const bool v = valid_;
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  return v;
}

const std::string& ConfigStore::path() const { return path_; }

alert_core::JsonValue ConfigStore::maskedDom() const {
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  auto out = maskDom(dom_, "");
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  return out;
}

alert_core::JsonValue ConfigStore::rawDom() const {
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  auto out = cloneDom(dom_);
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  return out;
}

bool ConfigStore::validateAndMerge(const std::string& body, alert_core::JsonValue& merged,
                                   FieldErrors& errs) const {
  using alert_core::JsonValue;
  if (body.size() > kMaxConfigBodyBytes) {
    addErr(errs, "", "config body exceeds 4KB");
    return false;
  }
  JsonValue patch;
  alert_core::JsonParseError perr;
  if (!alert_core::jsonParse(body, patch, perr)) {
    addErr(errs, "", std::string("invalid JSON at offset ") +
                      std::to_string(perr.offset) + ": " + perr.message);
    return false;
  }
  if (patch.type() != JsonValue::Type::Object) {
    addErr(errs, "", "config body must be a JSON object");
    return false;
  }

  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  const std::string current_wifi =
      objGet(dom_, "admin") && objGet(*objGet(dom_, "admin"), "wifi_list")
          ? objGet(*objGet(dom_, "admin"), "wifi_list")->toString()
          : "[]";

  for (const auto& kv : patch.objectItems()) {
    if (kv.first != "schema_version" && kv.first != "admin" &&
        kv.first != "alert_type_overrides" && kv.first != "mute_rules") {
      addErr(errs, kv.first, "unknown top-level field");
    }
  }
  if (const JsonValue* sv = objGet(patch, "schema_version")) {
    if (sv->type() != JsonValue::Type::Int || sv->intValue() != kSchemaVersion) {
      addErr(errs, "schema_version", "unsupported schema_version (expected 1)");
    }
  }
  if (const JsonValue* admin = objGet(patch, "admin")) {
    validateAdminSection(*admin, errs, current_wifi, true);
  }
  if (const JsonValue* ov = objGet(patch, "alert_type_overrides")) {
    validateOverridesSection(*ov, errs);
  }
  if (const JsonValue* mr = objGet(patch, "mute_rules")) {
    validateMuteRulesSection(*mr, errs);
  }
  if (!errs.empty()) {
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return false;
  }

  merged = cloneDom(dom_);
  for (const auto& kv : patch.objectItems()) {
    objSet(merged, kv.first, cloneDom(kv.second));
  }
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  return true;
}

bool ConfigStore::commit(const alert_core::JsonValue& merged, std::string& err) {
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  FieldErrors validation;
  if (!validateLoadedDom(merged, validation)) {
    err = "config validation failed; refusing to persist secret or invalid values";
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return false;
  }
  const std::string text = merged.toString();
  bool replaced = false;
  if (atomicConfigWrite(path_, text, replaced)) {
    dom_ = cloneDom(merged);
    admin_ = parseAdminView(dom_);
    valid_ = true;
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return true;
  }
  err = "atomic config write failed (rolled back to previous RAM copy)";
  ADMIN_LOG_ERROR("config commit failed: %s", path_.c_str());
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  return false;
}

AdminDomainConfig ConfigStore::admin() const {
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  const AdminDomainConfig out = admin_;
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  return out;
}

std::vector<alert_core::MuteRule> ConfigStore::muteRules() const {
  std::vector<alert_core::MuteRule> out;
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  if (const alert_core::JsonValue* mr = objGet(dom_, "mute_rules")) {
    out.reserve(mr->arrayItems().size());
    for (const auto& item : mr->arrayItems()) {
      alert_core::MuteRule r;
      std::string f, m;
      if (muteRuleFromJson(item, r, f, m)) out.push_back(std::move(r));
    }
  }
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  return out;
}

bool ConfigStore::setMuteRulesInDom(alert_core::JsonValue& dom,
                                    const std::vector<alert_core::MuteRule>& rules) const {
  using alert_core::JsonValue;
  JsonValue::Array arr;
  arr.reserve(rules.size());
  for (const auto& r : rules) arr.push_back(muteRuleToJson(r));
  objSet(dom, "mute_rules", JsonValue::makeArray(std::move(arr)));
  return true;
}

bool ConfigStore::setWifiListInDom(
    alert_core::JsonValue& dom,
    const std::vector<std::pair<std::string, std::string>>& ssid_ref_list) const {
  using alert_core::JsonValue;
  JsonValue::Array arr;
  arr.reserve(ssid_ref_list.size());
  for (const auto& e : ssid_ref_list) {
    arr.push_back(::eego::json::ObjectWriter(2)
        .string("ssid", e.first)
        .string("pass_ref", e.second)
        .take());
  }
  JsonValue* admin = nullptr;
  for (auto& kv : mutableItems(dom)) {
    if (kv.first == "admin") admin = &kv.second;
  }
  if (admin == nullptr || admin->type() != JsonValue::Type::Object) return false;
  objSet(*admin, "wifi_list", JsonValue::makeArray(std::move(arr)));
  return true;
}

const char* ConfigStore::schemaJson() {
  return R"JSON({
  "schema_version": 1,
  "description": "EEGO A4 device config v1 (data sources live in the dashboard bundle, not here). PUT replaces the top-level sections present in the body. Unknown top-level fields in the body are rejected; unknown fields inside stored sections are preserved. Secrets live only in NVS; this file carries *_ref references.",
  "top_level": {
    "schema_version": {"type": "int", "const": 1},
    "admin": {
      "type": "object",
      "fields": {
        "wifi_list": {"type": "array", "max_items": 8, "read_only": true,
                      "item": {"ssid": "string 1..32B", "pass_ref": "\"nvs:wifi/<idx>\" (managed via /api/v1/config/wifi; list order = connect priority)"}},
        "network": {"type": "object", "fields": {"auto_ap_fallback": {"type": "bool", "default": false}}},
        "security": {"type": "object", "fields": {
          "session_ttl_s": {"type": "int", "range": [300, 86400], "default": 86400},
          "login_lock_threshold": {"type": "int", "range": [3, 10], "default": 5}}},
        "display": {"type": "object", "fields": {
          "font": {"type": "string", "max_len": 128, "note": "absolute path, no '..'; empty = built-in"}}},
        "logging": {"type": "object", "fields": {
          "level": {"type": "string", "enum": ["ERROR", "WARN", "INFO", "DEBUG"], "default": "INFO"},
          "sd_log": {"type": "bool", "default": false}}}
      }
    },
    "alert_type_overrides": {"type": "object", "max_entries": 64,
      "value": {"severity": "critical|warning|info", "ttl_s": "int 0..31622400",
                "name_zh": "string <=64B", "name_en": "string <=64B", "muted": "bool"}},
    "mute_rules": {"type": "array", "max_items": 32, "description": "store-level mute rules (M21); hot-applied to net_task, restored on boot",
      "item": {"match_by": "\"type\"|\"source\"|\"fingerprint\"",
               "value": "type/source id (1..64B) or 40-hex fingerprint",
               "expires_at": "absolute unix seconds, 0/absent = never expires"}}
  }
})JSON";
}

}  // namespace eego::admin
