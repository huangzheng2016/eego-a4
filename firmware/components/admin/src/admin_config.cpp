// SPDX-License-Identifier: GPL-3.0-or-later
// 配置存储实现 —— 见 include/admin/admin_config.h。
#include "admin/admin_config.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#include "admin/admin_log.h"
#include "alert_core/types.h"
#include "device_settings/device_settings.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "net_stack/credentials.h"
#include "source_secret_migration.h"

namespace eego::admin {

namespace {

constexpr size_t kMaxConfigBodyBytes = 4 * 1024;  // M13：config body ≤4KB（httpd 强制）
constexpr size_t kMaxSources = 16;
constexpr size_t kMaxOverrides = 64;
constexpr size_t kMaxWifiList = 4;
constexpr size_t kMaxMuteRules = 32;              // M21：静音规则上限
constexpr int64_t kSchemaVersion = 1;
constexpr int64_t kMaxMuteExpiresAt = 4102444800LL;  // 2100-01-01T00:00:00Z（规则 TTL 上限）

// ---- DOM 小工具（alert_core::JsonValue 无可变查找，这里补齐）----
const alert_core::JsonValue* objGet(const alert_core::JsonValue& obj, const char* key) {
  return obj.find(key);
}

// JsonValue 只提供 const 访问器；值语义类型，const_cast 安全。
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
  if (keyEndsWith(key_lower, "_ref")) return false;  // *_ref 引用可显示
  if (key_lower.find("password") != std::string::npos ||
      key_lower.find("passwd") != std::string::npos ||
      key_lower.find("passphrase") != std::string::npos ||
      key_lower.find("secret") != std::string::npos ||
      key_lower.find("token") != std::string::npos) {
    return true;
  }
  if (key_lower == "key" || key_lower == "apikey" || key_lower == "api-key") return true;
  // *_key 后缀覆盖 api_key/access_key 等未来 schema 字段；
  // dedup_key 是去重字段名列表（非机密），显式豁免防过度掩码（review P2-5）
  if (keyEndsWith(key_lower, "_key") && key_lower != "dedup_key") return true;
  return false;
}

bool isSensitiveHeaderName(const std::string& name_lower) {
  return net::CredentialStore::sensitiveHeaderName(name_lower);
}

// 深拷贝 + secrets 掩码（M13：远程响应 secrets 省略或 "********"）。
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
          // sources.headers：按头名敏感程度掩码值
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

// ---- 校验工具 ----

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
  // "nvs:wifi/<idx>" 或空（空 = 无凭据引用）
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

// admin 域白名单校验（M13 §3）。current_wifi_list_json 非空时执行只读
// 一致性比较。strict_unknown=false（启动校验）时容忍未知字段（前向兼容，
// 字段保留但不应用）；true（PUT）时未知字段即错误。
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
      // wifi_list 只能经 /api/v1/config/wifi 变更；PUT 携带一致副本视为 no-op
      if (v.toString() != current_wifi_list_json) {
        addErr(errs, "admin.wifi_list",
               "read-only here; manage via /api/v1/config/wifi");
        continue;
      }
      if (v.arrayItems().size() > kMaxWifiList) {
        addErr(errs, "admin.wifi_list", "too many entries (max 4)");
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
        if (nkv.first == "brightness" || nkv.first == "warmth") {
          if (nkv.second.type() != JsonValue::Type::Int ||
              nkv.second.intValue() < 0 || nkv.second.intValue() > 100) {
            addErr(errs, "admin.display." + nkv.first, "must be an integer in [0, 100]");
          }
        } else if (nkv.first == "font") {
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

void validateSourcesSection(const alert_core::JsonValue& arr, FieldErrors& errs) {
  using alert_core::JsonValue;
  if (arr.type() != JsonValue::Type::Array) {
    addErr(errs, "sources", "must be an array");
    return;
  }
  if (arr.arrayItems().size() > kMaxSources) {
    addErr(errs, "sources", "too many sources (max 16)");
    return;
  }
  std::vector<std::string> ids;
  for (size_t i = 0; i < arr.arrayItems().size(); i++) {
    net::SourceConfig cfg;
    alert_core::ValidationError verr;
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "sources[%zu].", i);
    if (!net::SourceConfig::fromJson(arr.arrayItems()[i], cfg, verr)) {
      addErr(errs, prefix + verr.field, verr.message);
      continue;
    }
    const std::string sem = cfg.validate();
    if (!sem.empty()) {
      addErr(errs, std::string(prefix) + "url", sem);
    }
    for (const auto& id : ids) {
      if (id == cfg.id) {
        addErr(errs, std::string(prefix) + "id", "duplicate source id");
      }
    }
    ids.push_back(cfg.id);
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

// ---- mute_rules 节（M21）----
// JSON 形状：{"match_by": "type"|"source"|"fingerprint", "value": "...",
//            "expires_at": <unix 秒，可选，0/缺省 = 永不过期>}
// expires_at 用绝对 unix 秒而非相对 TTL：重启恢复后规则过期语义不变。
// 校验与 ConfigStore::muteRules 解析共用本函数；fingerprint 值归一为小写
// （store 指纹为小写 sha1 hex，规则必须同形才能命中）。

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
  // 第一遍：match_by（value 的形状校验依赖它；JSON 字段顺序不保证）
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
  // 第二遍：value / expires_at / 未知字段
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
  return JsonValue::makeObject({
      {"match_by", JsonValue::makeString(by)},
      {"value", JsonValue::makeString(r.value)},
      {"expires_at", JsonValue::makeInt(static_cast<int64_t>(r.expires_at))},
  });
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

// 从 DOM 解析 admin 域强类型视图（调用方已校验形状；这里宽容缺省）。
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
    if (const JsonValue* b = objGet(*v, "brightness")) {
      const int64_t t = b->intValue(out.brightness);
      if (t >= 0 && t <= 100) out.brightness = static_cast<int>(t);
    }
    if (const JsonValue* b = objGet(*v, "warmth")) {
      const int64_t t = b->intValue(out.warmth);
      if (t >= 0 && t <= 100) out.warmth = static_cast<int>(t);
    }
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
  JsonValue admin = JsonValue::makeObject({
      {"wifi_list", JsonValue::makeArray({})},
      {"network", JsonValue::makeObject({{"auto_ap_fallback", JsonValue::makeBool(false)}})},
      {"security", JsonValue::makeObject({
                        {"session_ttl_s", JsonValue::makeInt(86400)},
                        {"login_lock_threshold", JsonValue::makeInt(5)},
                    })},
      {"display", JsonValue::makeObject({
                       {"brightness", JsonValue::makeInt(0)},
                       {"warmth", JsonValue::makeInt(50)},
                       {"font", JsonValue::makeString("")},
                   })},
      {"logging", JsonValue::makeObject({
                       {"level", JsonValue::makeString("INFO")},
                       {"sd_log", JsonValue::makeBool(false)},
                   })},
  });
  return JsonValue::makeObject({
      {"schema_version", JsonValue::makeInt(kSchemaVersion)},
      {"admin", std::move(admin)},
      {"sources", JsonValue::makeArray({})},
      {"alert_type_overrides", JsonValue::makeObject({})},
      {"mute_rules", JsonValue::makeArray({})},
  });
}

// 启动校验：schema_version/admin/sources/alert_type_overrides 形状与取值。
// 未知顶层键容忍（保留策略）；admin 内未知键容忍（忽略但保留）。
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
  // 启动校验 wifi_list 条目（形状 + 值域；引用目标缺失不视为错误——NVS 可能还没写）
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
      if (e.field == "admin.wifi_list") continue;  // 只读一致性检查不适用启动
      errs.push_back(e);
    }
  }
  if (const JsonValue* src = objGet(dom, "sources")) {
    const size_t before = errs.size();
    validateSourcesSection(*src, errs);
    if (errs.size() != before) return false;
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

bool migrateLegacySourceSecrets(alert_core::JsonValue& dom,
                                internal::SourceSecretMigration& migration, bool& changed,
                                std::string& error) {
  using alert_core::JsonValue;
  JsonValue* sources = const_cast<JsonValue*>(objGet(dom, "sources"));
  if (sources == nullptr || sources->type() != JsonValue::Type::Array) return true;
  auto& items = const_cast<JsonValue::Array&>(sources->arrayItems());
  for (size_t i = 0; i < items.size(); ++i) {
    JsonValue& source = items[i];
    if (source.type() != JsonValue::Type::Object) continue;
    JsonValue* headers = const_cast<JsonValue*>(objGet(source, "headers"));
    if (headers == nullptr || headers->type() != JsonValue::Type::Object) continue;
    JsonValue* existing_refs = const_cast<JsonValue*>(objGet(source, "header_secret_refs"));
    if (existing_refs != nullptr && existing_refs->type() != JsonValue::Type::Object) {
      error = "legacy header_secret_refs must be an object";
      return false;
    }
    JsonValue::Object plain;
    JsonValue::Object refs;
    if (existing_refs != nullptr) refs = existing_refs->objectItems();
    const JsonValue* id_value = objGet(source, "id");
    const std::string id = id_value != nullptr && id_value->type() == JsonValue::Type::String
                               ? id_value->stringValue()
                               : std::to_string(i);
    for (const auto& header : headers->objectItems()) {
      if (header.second.type() != JsonValue::Type::String) {
        plain.emplace_back(header.first, header.second);
        continue;
      }
      std::string lowered = header.first;
      for (char& c : lowered) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      }
      const bool authorization = lowered == "authorization";
      if (!authorization && !net::CredentialStore::sensitiveHeaderName(header.first)) {
        plain.emplace_back(header.first, header.second);
        continue;
      }
      if (existing_refs != nullptr && existing_refs->find(header.first) != nullptr) {
        error = "legacy plain and referenced secret headers are ambiguous";
        return false;
      }
      std::string secret = header.second.stringValue();
      if (authorization) {
        const JsonValue* existing_auth = objGet(source, "auth_token_ref");
        if (existing_auth != nullptr &&
            (existing_auth->type() != JsonValue::Type::String ||
             !existing_auth->stringValue().empty())) {
          net::CredentialStore::secureErase(secret);
          error = "legacy Authorization conflicts with auth_token_ref";
          return false;
        }
        std::string scheme = secret.substr(0, 7);
        for (char& c : scheme) {
          if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        if (secret.size() < 8 || scheme != "bearer ") {
          net::CredentialStore::secureErase(secret);
          error = "legacy Authorization must use Bearer; re-enter it via credentials API";
          return false;
        }
        secret = secret.substr(7);
      }
      const std::string ref = migration.add("legacy/" + id, header.first, secret, error);
      net::CredentialStore::secureErase(secret);
      if (ref.empty()) return false;
      if (authorization) {
        objSet(source, "auth_token_ref", JsonValue::makeString(ref));
      } else {
        refs.emplace_back(header.first, JsonValue::makeString(ref));
      }
      changed = true;
    }
    objSet(source, "headers", JsonValue::makeObject(std::move(plain)));
    if (!refs.empty()) objSet(source, "header_secret_refs", JsonValue::makeObject(std::move(refs)));
  }
  return true;
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
  const std::string tmp = path + ".tmp";
  bool ok = false;
  if (FILE* f = fopen(tmp.c_str(), "wb")) {
    ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    if (ok) ok = fflush(f) == 0;
    if (ok) ok = fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = false;
  }
  if (ok) {
    ok = rename(tmp.c_str(), path.c_str()) == 0;
    replaced = ok;
  }
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
  if (!ok && !replaced) remove(tmp.c_str());
  return ok;
}

}  // namespace

void validateLegacyDisplayCapability(const alert_core::JsonValue& display_patch,
                                     FieldErrors& errors) {
  const settings::Snapshot snap = settings::DeviceSettings::instance().snapshot();
  settings::FieldErrors capability_errors;
  settings::validateLegacyDisplayCapability(display_patch, snap.values,
                                            snap.capabilities.frontlight, capability_errors);
  for (const auto& error : capability_errors) errors.push_back({error.field, error.message});
}

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

  // 读文件
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
    bool migration_failed = false;
    if (parse_ok) {
      internal::SourceSecretMigration migration;
      bool migrated = false;
      bool replaced = false;
      std::string migration_error;
      bool migration_ok = migrateLegacySourceSecrets(parsed, migration, migrated, migration_error);
      if (migration_ok) migration_ok = migration.persist(migration_error);
      if (migration_ok && migrated) {
        migration_ok = atomicConfigWrite(path_, parsed.toString(), replaced);
      }
      if (!migration_ok) {
        if (!replaced) migration.rollback();
        migration_failed = true;
        valid_ = false;
        ADMIN_LOG_ERROR("config secret migration failed; configuration not loaded");
      }
      migration.finish();
      if (!migration_failed) {
        validateLoadedDom(parsed, errs);
        if (errs.empty()) {
          dom_ = std::move(parsed);
          admin_ = parseAdminView(dom_);
        } else {
          valid_ = false;
        }
      }
    } else {
      valid_ = false;
    }
    if (!valid_ && !migration_failed) {
      // 坏文件改名 config.bak（M13 F6），加载默认；迁移失败时保留原路径以便明确恢复。
      const std::string bak = path_ + ".bak";
      remove(bak.c_str());
      if (rename(path_.c_str(), bak.c_str()) != 0) {
        ADMIN_LOG_ERROR("config corrupt; backup rename failed (%s)", path_.c_str());
      } else {
        ADMIN_LOG_ERROR("config corrupt; backed up to config.bak, loading defaults");
      }
    }
  } else if (!read_ok) {
    // 文件存在但读取失败 → 视为损坏
    if (FILE* f = fopen(path_.c_str(), "rb")) {
      fclose(f);
      valid_ = false;
    }
    // 文件不存在 → 新设备，默认配置（valid=true）
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

  // 顶层白名单（M13：未知顶层字段拒绝）；文件内未知键保留不动
  for (const auto& kv : patch.objectItems()) {
    if (kv.first != "schema_version" && kv.first != "admin" && kv.first != "sources" &&
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
  if (const JsonValue* src = objGet(patch, "sources")) {
    validateSourcesSection(*src, errs);
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

  // 合并：出现的顶层 section 整体替换，未出现的保留
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

std::vector<net::SourceConfig> ConfigStore::sources() const {
  std::vector<net::SourceConfig> out;
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  if (const alert_core::JsonValue* src = objGet(dom_, "sources")) {
    out.reserve(src->arrayItems().size());
    for (const auto& item : src->arrayItems()) {
      net::SourceConfig cfg;
      alert_core::ValidationError verr;
      if (net::SourceConfig::fromJson(item, cfg, verr)) out.push_back(std::move(cfg));
    }
  }
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
      // DOM 已经过启动/PUT 校验；非法条目防御性跳过（不放大故障）
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
    arr.push_back(JsonValue::makeObject({
        {"ssid", JsonValue::makeString(e.first)},
        {"pass_ref", JsonValue::makeString(e.second)},
    }));
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
  // GET /config/schema 静态响应（M13 §3 admin 域 v1 + sources/overrides 引用）。
  return R"JSON({
  "schema_version": 1,
  "description": "EEGO A4 alert panel config v1. PUT replaces the top-level sections present in the body. Unknown top-level fields in the body are rejected; unknown fields inside stored sections are preserved. Secrets live only in NVS; this file carries *_ref references.",
  "top_level": {
    "schema_version": {"type": "int", "const": 1},
    "admin": {
      "type": "object",
      "fields": {
        "wifi_list": {"type": "array", "max_items": 4, "read_only": true,
                      "item": {"ssid": "string 1..32B", "pass_ref": "\"nvs:wifi/<idx>\" (managed via /api/v1/config/wifi)"}},
        "network": {"type": "object", "fields": {"auto_ap_fallback": {"type": "bool", "default": false}}},
        "security": {"type": "object", "fields": {
          "session_ttl_s": {"type": "int", "range": [300, 86400], "default": 86400},
          "login_lock_threshold": {"type": "int", "range": [3, 10], "default": 5}}},
        "display": {"type": "object", "fields": {
          "brightness": {"type": "int", "range": [0, 100], "default": 0},
          "warmth": {"type": "int", "range": [0, 100], "default": 50},
          "font": {"type": "string", "max_len": 128, "note": "absolute path, no '..'; empty = built-in"}}},
        "logging": {"type": "object", "fields": {
          "level": {"type": "string", "enum": ["ERROR", "WARN", "INFO", "DEBUG"], "default": "INFO"},
          "sd_log": {"type": "bool", "default": false}}}
      }
    },
    "sources": {"type": "array", "max_items": 16,
      "auth_token_ref": "\"nvs:source/<key>\"; manage via /api/v1/sources/credentials",
      "headers": "object of non-secret literal values only",
      "header_secret_refs": "object of header name -> \"nvs:source/<key>\"; Authorization uses auth_token_ref"},
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
