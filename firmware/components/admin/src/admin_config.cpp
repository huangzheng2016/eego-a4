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
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace eego::admin {

namespace {

constexpr size_t kMaxConfigBodyBytes = 4 * 1024;  // M13：config body ≤4KB（httpd 强制）
constexpr size_t kMaxSources = 16;
constexpr size_t kMaxOverrides = 64;
constexpr size_t kMaxWifiList = 4;
constexpr int64_t kSchemaVersion = 1;

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
  return key_lower.find("password") != std::string::npos ||
         key_lower.find("secret") != std::string::npos ||
         key_lower.find("token") != std::string::npos ||
         key_lower == "key";
}

bool isSensitiveHeaderName(const std::string& name_lower) {
  return name_lower.find("auth") != std::string::npos ||
         name_lower.find("key") != std::string::npos ||
         name_lower.find("token") != std::string::npos ||
         name_lower.find("pass") != std::string::npos ||
         name_lower.find("secret") != std::string::npos;
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
        if (lower == "headers" && kv.second.type() == JsonValue::Type::Object) {
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
  return errs.empty();
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
    if (alert_core::jsonParse(text, parsed, perr)) {
      validateLoadedDom(parsed, errs);
      if (errs.empty()) {
        dom_ = std::move(parsed);
        admin_ = parseAdminView(dom_);
      } else {
        valid_ = false;
      }
    } else {
      valid_ = false;
    }
    if (!valid_) {
      // 坏文件改名 config.bak（M13 F6），加载默认
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
        kv.first != "alert_type_overrides") {
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

  const std::string text = merged.toString();
  const std::string tmp = path_ + ".tmp";

  // 确保父目录存在（/sdcard/alertpanel）
  const size_t slash = path_.find_last_of('/');
  if (slash != std::string::npos && slash > 0) {
    const std::string dir = path_.substr(0, slash);
    if (mkdir(dir.c_str(), 0775) != 0 && errno != EEXIST) {
      // FATFS VFS mkdir 已存在时返回 EEXIST；其他错误继续尝试写（fopen 会再失败）
    }
  }

  bool ok = false;
  if (FILE* f = fopen(tmp.c_str(), "wb")) {
    ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    if (ok) ok = (fflush(f) == 0);
    if (ok) ok = (fsync(fileno(f)) == 0);
    if (fclose(f) != 0) ok = false;
  }
  if (ok) {
    ok = (rename(tmp.c_str(), path_.c_str()) == 0);
  }
  if (ok) {
    // 回读校验（M13 F6：回读验证，失败回滚）
    std::string back;
    if (FILE* f = fopen(path_.c_str(), "rb")) {
      char buf[512];
      size_t n;
      while ((n = fread(buf, 1, sizeof(buf), f)) > 0) back.append(buf, n);
      fclose(f);
    }
    alert_core::JsonValue rp;
    alert_core::JsonParseError perr;
    if (!back.empty() && alert_core::jsonParse(back, rp, perr) &&
        rp.toString() == merged.toString()) {
      dom_ = cloneDom(merged);
      admin_ = parseAdminView(dom_);
      valid_ = true;
      xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
      return true;
    }
    ok = false;
  }
  remove(tmp.c_str());
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
    "sources": {"type": "array", "max_items": 16, "see": "net_stack SourceConfig (M11 §5)"},
    "alert_type_overrides": {"type": "object", "max_entries": 64,
      "value": {"severity": "critical|warning|info", "ttl_s": "int 0..31622400",
                "name_zh": "string <=64B", "name_en": "string <=64B", "muted": "bool"}}
  }
})JSON";
}

}  // namespace eego::admin
