// SPDX-License-Identifier: GPL-3.0-or-later
#include "admin/dashboard_config.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

#include "admin/admin_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace eego::admin {
namespace {

using dashboard::JsonValue;
using dashboard::ValidationError;
using dashboard::pipeline::SourcePlan;

constexpr char kMask[] = "********";

void addError(std::vector<ValidationError>& errors, const std::string& field,
              const std::string& message) {
  errors.push_back({field, message});
}

std::string lower(std::string_view value) {
  std::string out(value);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool endsWith(const std::string& value, const char* suffix) {
  const size_t length = std::strlen(suffix);
  return value.size() >= length &&
         value.compare(value.size() - length, length, suffix) == 0;
}

bool sensitiveKey(const std::string& key) {
  const std::string k = lower(key);
  if (endsWith(k, "_ref")) return false;
  return k.find("password") != std::string::npos || k.find("passwd") != std::string::npos ||
         k.find("passphrase") != std::string::npos || k.find("secret") != std::string::npos ||
         k.find("token") != std::string::npos || k.find("credential") != std::string::npos ||
         k == "key" || k == "apikey" || k == "api-key" ||
         (endsWith(k, "_key") && k != "dedup_key") || k == "authorization" ||
         k == "cookie" || k == "set-cookie";
}

bool sensitiveHeader(const std::string& key) {
  const std::string k = lower(key);
  return k.find("auth") != std::string::npos || k.find("key") != std::string::npos ||
         k.find("token") != std::string::npos || k.find("pass") != std::string::npos ||
         k.find("secret") != std::string::npos || k.find("cookie") != std::string::npos;
}

JsonValue maskJson(const JsonValue& value) {
  if (value.type() == JsonValue::Type::Array) {
    JsonValue::Array out;
    out.reserve(value.arrayItems().size());
    for (const auto& item : value.arrayItems()) out.push_back(maskJson(item));
    return JsonValue::makeArray(std::move(out));
  }
  if (value.type() != JsonValue::Type::Object) return value;
  JsonValue::Object out;
  out.reserve(value.objectItems().size());
  for (const auto& member : value.objectItems()) {
    if (member.first == "headers" && member.second.type() == JsonValue::Type::Object) {
      JsonValue::Object headers;
      for (const auto& header : member.second.objectItems()) {
        if (header.second.type() == JsonValue::Type::String && sensitiveHeader(header.first)) {
          headers.emplace_back(header.first, JsonValue::makeString(kMask));
        } else {
          headers.emplace_back(header.first, maskJson(header.second));
        }
      }
      out.emplace_back(member.first, JsonValue::makeObject(std::move(headers)));
    } else if (member.second.type() == JsonValue::Type::String && sensitiveKey(member.first)) {
      out.emplace_back(member.first, JsonValue::makeString(kMask));
    } else {
      out.emplace_back(member.first, maskJson(member.second));
    }
  }
  return JsonValue::makeObject(std::move(out));
}

void set(JsonValue& object, const std::string& key, JsonValue value) {
  if (JsonValue* current = object.find(key)) {
    *current = std::move(value);
    return;
  }
  object.objectItems().emplace_back(key, std::move(value));
}

JsonValue sourceToJson(const dashboard::Source& source) {
  JsonValue::Object headers;
  for (const auto& header : source.headers) {
    headers.emplace_back(header.key, JsonValue::makeString(header.value));
  }
  return JsonValue::makeObject({
      {"id", JsonValue::makeString(source.id)},
      {"kind", JsonValue::makeString(dashboard::sourceKindName(source.kind))},
      {"enabled", JsonValue::makeBool(source.enabled)},
      {"url", JsonValue::makeString(source.url)},
      {"refresh_ms", JsonValue::makeInt(source.refresh_ms)},
      {"timeout_ms", JsonValue::makeInt(source.timeout_ms)},
      {"max_response_bytes", JsonValue::makeInt(source.max_response_bytes)},
      {"tls_verify", JsonValue::makeBool(source.tls_verify)},
      {"auth_token_ref", JsonValue::makeString(source.auth_token_ref)},
      {"headers", JsonValue::makeObject(std::move(headers))},
  });
}

void syncPlanSource(JsonValue& plan, const dashboard::Source& source) {
  if (plan.type() != JsonValue::Type::Object) return;
  const JsonValue canonical = sourceToJson(source);
  for (const auto& member : canonical.objectItems()) set(plan, member.first, member.second);
}

const dashboard::Source* findSource(const DashboardDocument& document, const std::string& id) {
  for (const auto& source : document.dashboard.sources) {
    if (source.id == id) return &source;
  }
  return nullptr;
}

const SourcePlan* findPlan(const DashboardDocument& document, const std::string& id) {
  for (const auto& plan : document.plans) {
    if (plan.source.id == id) return &plan;
  }
  return nullptr;
}

const char* storedHeader(const std::vector<dashboard::KeyValue>& headers,
                         const std::string& name) {
  for (const auto& header : headers) {
    if (lower(header.key) == lower(name)) return header.value.c_str();
  }
  return nullptr;
}

bool restoreSourceSecrets(dashboard::Source& source, const DashboardDocument* current,
                          const std::string& path, std::vector<ValidationError>& errors) {
  bool ok = true;
  const dashboard::Source* previous = current == nullptr ? nullptr : findSource(*current, source.id);
  for (auto& header : source.headers) {
    if (header.value != kMask) continue;
    const char* stored = previous == nullptr ? nullptr : storedHeader(previous->headers, header.key);
    if (stored == nullptr) {
      addError(errors, path + ".headers." + header.key,
               "masked header has no stored value; provide a new value or remove it");
      ok = false;
    } else {
      header.value = stored;
    }
  }
  return ok;
}

bool restorePlanSecrets(SourcePlan& plan, const DashboardDocument* current,
                        const std::string& path, std::vector<ValidationError>& errors) {
  bool ok = true;
  const SourcePlan* previous = current == nullptr ? nullptr : findPlan(*current, plan.source.id);
  for (auto& header : plan.source.headers) {
    if (header.value != kMask) continue;
    const char* stored = previous == nullptr ? nullptr : storedHeader(previous->source.headers, header.key);
    if (stored == nullptr) {
      addError(errors, path + ".headers." + header.key,
               "masked header has no stored value; provide a new value or remove it");
      ok = false;
    } else {
      header.value = stored;
    }
  }
  return ok;
}

bool sameSource(const dashboard::Source& left, const dashboard::Source& right) {
  if (left.id != right.id || left.kind != right.kind || left.enabled != right.enabled ||
      left.url != right.url || left.refresh_ms != right.refresh_ms ||
      left.timeout_ms != right.timeout_ms ||
      left.max_response_bytes != right.max_response_bytes ||
      left.tls_verify != right.tls_verify || left.auth_token_ref != right.auth_token_ref ||
      left.headers.size() != right.headers.size()) {
    return false;
  }
  for (const auto& header : left.headers) {
    const char* value = storedHeader(right.headers, header.key);
    if (value == nullptr || header.value != value) return false;
  }
  return true;
}

bool readFile(const std::string& path, size_t maximum, std::string& out, bool& exists) {
  exists = false;
  out.clear();
  FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) return errno == ENOENT;
  exists = true;
  char buffer[1024];
  bool ok = true;
  size_t count = 0;
  while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    out.append(buffer, count);
    if (out.size() > maximum) {
      ok = false;
      break;
    }
  }
  if (std::ferror(file)) ok = false;
  if (std::fclose(file) != 0) ok = false;
  return ok;
}

bool atomicWrite(const std::string& path, const std::string& text, std::string& error) {
  const size_t slash = path.find_last_of('/');
  if (slash != std::string::npos && slash > 0) {
    const std::string directory = path.substr(0, slash);
    if (mkdir(directory.c_str(), 0775) != 0 && errno != EEXIST) {
      // fopen below returns the authoritative error without exposing file contents.
    }
  }
  const std::string temporary = path + ".tmp";
  bool ok = false;
  if (FILE* file = std::fopen(temporary.c_str(), "wb")) {
    ok = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    if (ok) ok = std::fflush(file) == 0;
    if (ok) ok = fsync(fileno(file)) == 0;
    if (std::fclose(file) != 0) ok = false;
  }
  if (ok) ok = std::rename(temporary.c_str(), path.c_str()) == 0;
  if (ok) {
    std::string verify;
    bool exists = false;
    ok = readFile(path, kDashboardBundleMaxBytes, verify, exists) && exists && verify == text;
  }
  if (!ok) {
    std::remove(temporary.c_str());
    error = "atomic dashboard write failed; previous in-memory configuration retained";
    return false;
  }
  return true;
}

}  // namespace

DashboardStore& DashboardStore::instance() {
  static DashboardStore store;
  return store;
}

esp_err_t DashboardStore::init(const std::string& path) {
  if (mutex_ == nullptr) mutex_ = xSemaphoreCreateMutex();
  if (mutex_ == nullptr) return ESP_ERR_NO_MEM;
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  path_ = path;
  valid_ = true;
  auto empty = std::make_shared<DashboardDocument>();
  snapshot_ = empty;

  std::string text;
  bool exists = false;
  const bool read_ok = readFile(path_, kDashboardBundleMaxBytes, text, exists);
  if (read_ok && exists && !text.empty()) {
    DashboardParseResult parsed = parse(text);
    if (parsed.ok()) {
      parsed.document.configured = true;
      if (parsed.document.revision == 0) parsed.document.revision = 1;
      snapshot_ = std::make_shared<DashboardDocument>(std::move(parsed.document));
    } else {
      valid_ = false;
    }
  } else if (!read_ok) {
    valid_ = false;
  }
  if (!valid_ && exists) {
    const std::string backup = path_ + ".bak";
    std::remove(backup.c_str());
    if (std::rename(path_.c_str(), backup.c_str()) == 0) {
      ADMIN_LOG_ERROR("dashboard config invalid; backed up and left unconfigured");
    } else {
      ADMIN_LOG_ERROR("dashboard config invalid; backup rename failed");
    }
  }
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  return ESP_OK;
}

bool DashboardStore::valid() const {
  if (mutex_ == nullptr) return valid_;
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  const bool value = valid_;
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  return value;
}

std::shared_ptr<const DashboardDocument> DashboardStore::snapshot() const {
  if (mutex_ == nullptr) {
    static const auto empty = std::make_shared<DashboardDocument>();
    return snapshot_ == nullptr ? empty : snapshot_;
  }
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  auto current = snapshot_;
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  if (current == nullptr) current = std::make_shared<DashboardDocument>();
  return current;
}

DashboardParseResult DashboardStore::parse(const std::string& json,
                                           const DashboardDocument* current) {
  DashboardParseResult result;
  if (json.size() > kDashboardBundleMaxBytes) {
    addError(result.errors, "", "dashboard bundle exceeds 192 KiB");
    return result;
  }
  JsonValue root;
  dashboard::JsonParseError parse_error;
  if (!dashboard::jsonParse(json, root, parse_error, 18)) {
    addError(result.errors, "", "JSON at byte " + std::to_string(parse_error.offset) + ": " +
                                    parse_error.message);
    return result;
  }
  if (root.type() != JsonValue::Type::Object) {
    addError(result.errors, "", "dashboard bundle must be an object");
    return result;
  }

  const JsonValue* dashboard_value = &root;
  const JsonValue* schema = root.find("schema");
  const bool bundle = root.find("dashboard") != nullptr ||
                      (schema != nullptr && schema->type() == JsonValue::Type::String &&
                       schema->stringValue() == kDashboardBundleSchema);
  if (bundle) {
    for (const auto& member : root.objectItems()) {
      if (member.first != "schema" && member.first != "schema_version" &&
          member.first != "revision" && member.first != "dashboard" &&
          member.first != "source_plans") {
        addError(result.errors, member.first, "unknown bundle field");
      }
    }
    if (schema == nullptr || schema->type() != JsonValue::Type::String ||
        schema->stringValue() != kDashboardBundleSchema) {
      addError(result.errors, "schema", "expected eego.dashboard-bundle/1");
    }
    const JsonValue* version = root.find("schema_version");
    if (version == nullptr || version->type() != JsonValue::Type::Int ||
        version->intValue() != kDashboardBundleVersion) {
      addError(result.errors, "schema_version", "expected bundle schema_version 1");
    }
    if (const JsonValue* revision = root.find("revision")) {
      if (revision->type() != JsonValue::Type::Int || revision->intValue() < 0) {
        addError(result.errors, "revision", "must be a non-negative integer");
      } else {
        result.document.revision = static_cast<uint64_t>(revision->intValue());
      }
    }
    dashboard_value = root.find("dashboard");
    if (dashboard_value == nullptr) addError(result.errors, "dashboard", "missing required field");
    if (const JsonValue* plans = root.find("source_plans")) {
      result.document.source_plan_values = *plans;
    }
    if (!result.errors.empty()) return result;
  }

  dashboard::ParseResult parsed = dashboard::dashboardFromJson(*dashboard_value);
  result.source_version = parsed.source_version;
  result.migrated = parsed.migrated;
  for (const auto& error : parsed.errors) {
    const std::string prefix = bundle ? "dashboard." : "";
    addError(result.errors, prefix + error.field, error.message);
  }
  if (!parsed.ok()) return result;
  result.document.dashboard = std::move(parsed.dashboard);

  JsonValue& plans = result.document.source_plan_values;
  if (plans.isNull()) plans = JsonValue::makeArray({});
  if (plans.type() != JsonValue::Type::Array) {
    addError(result.errors, "source_plans", "must be an array");
    return result;
  }
  if (plans.arrayItems().size() > dashboard::Limits{}.max_sources) {
    addError(result.errors, "source_plans", "exceeds maximum source count");
    return result;
  }
  for (size_t i = 0; i < plans.arrayItems().size(); ++i) {
    const JsonValue& value = plans.arrayItems()[i];
    if (value.toString().size() > kDashboardSourcePlanMaxBytes) {
      addError(result.errors, "source_plans[" + std::to_string(i) + "]",
               "source plan exceeds 32 KiB");
      continue;
    }
    SourcePlan plan;
    std::string error;
    if (!dashboard::pipeline::sourcePlanFromJson(value, plan, error)) {
      addError(result.errors, "source_plans[" + std::to_string(i) + "]", error);
      continue;
    }
    result.document.plans.push_back(std::move(plan));
  }
  if (!result.errors.empty()) return result;

  result.document.configured = true;
  validate(result.document, current, result.errors);
  return result;
}

bool DashboardStore::validate(DashboardDocument& candidate, const DashboardDocument* current,
                              std::vector<ValidationError>& errors) {
  candidate.configured = true;
  dashboard::validateDashboard(candidate.dashboard, dashboard::Limits{}, errors);

  for (size_t i = 0; i < candidate.dashboard.sources.size(); ++i) {
    restoreSourceSecrets(candidate.dashboard.sources[i], current,
                         "dashboard.sources[" + std::to_string(i) + "]", errors);
  }

  if (candidate.source_plan_values.type() != JsonValue::Type::Array ||
      candidate.source_plan_values.arrayItems().size() > dashboard::Limits{}.max_sources) {
    addError(errors, "source_plans", "must be a bounded array");
    return false;
  }
  std::vector<SourcePlan> parsed_plans;
  for (size_t i = 0; i < candidate.source_plan_values.arrayItems().size(); ++i) {
    JsonValue& value = candidate.source_plan_values.arrayItems()[i];
    if (value.toString().size() > kDashboardSourcePlanMaxBytes) {
      addError(errors, "source_plans[" + std::to_string(i) + "]", "source plan exceeds 32 KiB");
      continue;
    }
    SourcePlan plan;
    std::string error;
    if (!dashboard::pipeline::sourcePlanFromJson(value, plan, error)) {
      addError(errors, "source_plans[" + std::to_string(i) + "]", error);
      continue;
    }
    restorePlanSecrets(plan, current, "source_plans[" + std::to_string(i) + "]", errors);
    syncPlanSource(value, plan.source);
    for (size_t j = 0; j < parsed_plans.size(); ++j) {
      if (parsed_plans[j].source.id == plan.source.id) {
        addError(errors, "source_plans[" + std::to_string(i) + "].id", "duplicate source plan");
      }
    }
    const dashboard::Source* source = findSource(candidate, plan.source.id);
    if (source == nullptr) {
      addError(errors, "source_plans[" + std::to_string(i) + "].id",
               "source plan requires a matching dashboard source");
    } else if (!sameSource(*source, plan.source)) {
      addError(errors, "source_plans[" + std::to_string(i) + "]",
               "plan connection fields must match the dashboard source");
    }
    parsed_plans.push_back(std::move(plan));
  }
  candidate.plans = std::move(parsed_plans);
  if (errors.empty() && toJson(candidate, false, false).toString().size() > kDashboardBundleMaxBytes) {
    addError(errors, "", "serialized dashboard bundle exceeds 192 KiB");
  }
  return errors.empty();
}

JsonValue DashboardStore::toJson(const DashboardDocument& document, bool mask_secrets,
                                 bool dashboard_only) {
  JsonValue output = dashboard::dashboardToJson(document.dashboard);
  if (!dashboard_only) {
    JsonValue::Object bundle;
    bundle.emplace_back("schema", JsonValue::makeString(kDashboardBundleSchema));
    bundle.emplace_back("schema_version", JsonValue::makeInt(kDashboardBundleVersion));
    bundle.emplace_back("revision", JsonValue::makeInt(static_cast<int64_t>(document.revision)));
    bundle.emplace_back("dashboard", std::move(output));
    bundle.emplace_back("source_plans", document.source_plan_values);
    output = JsonValue::makeObject(std::move(bundle));
  }
  return mask_secrets ? maskJson(output) : output;
}

bool DashboardStore::commit(const DashboardDocument& candidate, bool check_revision,
                            uint64_t expected_revision, std::string& error, bool& conflict) {
  conflict = false;
  error.clear();
  auto current = snapshot();
  DashboardDocument next = candidate;
  std::vector<ValidationError> validation_errors;
  if (!validate(next, current.get(), validation_errors)) {
    error = "dashboard revalidation failed";
    if (!validation_errors.empty()) {
      error += ": " + validation_errors.front().field + " " +
               validation_errors.front().message;
    }
    return false;
  }

  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  const uint64_t current_revision = snapshot_ == nullptr ? 0 : snapshot_->revision;
  if (check_revision && current_revision != expected_revision) {
    conflict = true;
    error = "dashboard revision conflict";
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return false;
  }
  if (current_revision == std::numeric_limits<uint64_t>::max()) {
    error = "dashboard revision exhausted";
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return false;
  }
  next.revision = current_revision + 1;
  next.configured = true;
  const std::string text = toJson(next, false, false).toString();
  if (text.size() > kDashboardBundleMaxBytes) {
    error = "dashboard bundle exceeds 192 KiB";
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return false;
  }
  if (!atomicWrite(path_, text, error)) {
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return false;
  }
  snapshot_ = std::make_shared<DashboardDocument>(std::move(next));
  valid_ = true;
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  ADMIN_LOG_INFO("dashboard config committed");
  return true;
}

bool DashboardStore::erase(bool check_revision, uint64_t expected_revision,
                           std::string& error, bool& conflict) {
  conflict = false;
  error.clear();
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  const uint64_t current_revision = snapshot_ == nullptr ? 0 : snapshot_->revision;
  if (check_revision && current_revision != expected_revision) {
    conflict = true;
    error = "dashboard revision conflict";
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return false;
  }
  if (std::remove(path_.c_str()) != 0 && errno != ENOENT) {
    error = "failed to remove dashboard configuration";
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return false;
  }
  auto empty = std::make_shared<DashboardDocument>();
  empty->revision = current_revision + 1;
  snapshot_ = std::move(empty);
  valid_ = true;
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  ADMIN_LOG_WARN("dashboard config removed");
  return true;
}

const char* DashboardStore::sampleJson() {
  return R"JSON({
  "schema":"eego.dashboard-bundle/1",
  "schema_version":1,
  "revision":0,
  "dashboard":{
    "schema":"eego.dashboard/2","schema_version":2,
    "id":"sample","title":"EEGO 数据面板 / Data dashboard","default_page_id":"main","refresh_ms":60000,
    "sources":[{"id":"sample","kind":"static","enabled":true,"url":"","refresh_ms":60000,"timeout_ms":10000,"max_response_bytes":65536,"tls_verify":true,"auth_token_ref":"","headers":{}}],
    "variables":[
      {"name":"account","label":"账号 / Account","kind":"text","default_value":"demo-account","options":[],"source_id":"","value_path":"","multi":false,"include_all":false},
      {"name":"city","label":"城市 / City","kind":"constant","default_value":"Shanghai","options":[{"label":"上海 / Shanghai","value":"Shanghai"},{"label":"深圳 / Shenzhen","value":"Shenzhen"}],"source_id":"","value_path":"","multi":false,"include_all":false},
      {"name":"server","label":"服务器 / Server","kind":"text","default_value":"srv-demo","options":[],"source_id":"","value_path":"","multi":false,"include_all":false},
      {"name":"time_range","label":"时间范围 / Time range","kind":"time_range","default_value":"-1h","options":[],"source_id":"","value_path":"","multi":false,"include_all":false},
      {"name":"locale","label":"语言 / Locale","kind":"locale","default_value":"zh-CN","options":[],"source_id":"","value_path":"","multi":false,"include_all":false}
    ],
    "pages":[
      {"id":"main","title":"总览 / Overview","visible":true,"grid_columns":24,"row_height":16,"panels":[
        {"id":"intro","kind":"text","grid":{"x":0,"y":0,"w":24,"h":4},"title":"说明 / About","visible":true,"source_id":"","metric":"","value_path":"","value_expression":"","time_window":{"from":"-1h","to":"now"},"refresh_ms":60000,"thresholds":[],"style":{"unit":"","variant":"multiline","precision":2,"line_width":1,"show_legend":true,"invert":false},"linked_panel_ids":[],"actions":[],"text":{"content":"静态样板数据，无需联网。\nStatic sample data; no network required.","multiline":true,"max_lines":4}},
        {"id":"token_card","kind":"card","grid":{"x":0,"y":4,"w":8,"h":6},"title":"TOKEN 用量 / Usage","visible":true,"source_id":"sample","metric":"token.used","value_path":"$.value","value_expression":"","time_window":{"from":"-1h","to":"now"},"refresh_ms":60000,"thresholds":[{"value":1000,"severity":"warning","label":"高 / High"}],"style":{"unit":"tokens","variant":"default","precision":0,"line_width":1,"show_legend":true,"invert":false},"linked_panel_ids":[],"actions":[],"text":{"content":"${account}","multiline":false,"max_lines":1}},
        {"id":"cpu_trend","kind":"chart","grid":{"x":8,"y":4,"w":16,"h":10},"title":"CPU 趋势 / CPU trend","visible":true,"source_id":"sample","metric":"server.cpu","value_path":"$.value","value_expression":"","time_window":{"from":"-1h","to":"now"},"refresh_ms":60000,"thresholds":[{"value":80,"severity":"critical","label":"过载 / Overload"}],"style":{"unit":"percent","variant":"default","precision":1,"line_width":2,"show_legend":true,"invert":false},"linked_panel_ids":[],"actions":[],"chart":{"kind":"line","show_points":true,"fill":false,"y_min":0,"y_max":100}},
        {"id":"main_actions","kind":"button","grid":{"x":0,"y":10,"w":8,"h":4},"title":"操作 / Actions","visible":true,"source_id":"","metric":"","value_path":"","value_expression":"","time_window":{"from":"-1h","to":"now"},"refresh_ms":60000,"thresholds":[],"style":{"unit":"","variant":"default","precision":2,"line_width":1,"show_legend":true,"invert":false},"linked_panel_ids":[],"actions":[{"id":"details","kind":"goto_page","label":"详情 / Details","target":"details","value":"","confirm":false},{"id":"refresh","kind":"refresh","label":"刷新 / Refresh","target":"current","value":"","confirm":false},{"id":"shanghai","kind":"set_variable","label":"上海 / Shanghai","target":"city","value":"Shanghai","confirm":true}]},
        {"id":"alert_preview","kind":"alert","grid":{"x":0,"y":14,"w":24,"h":5},"title":"告警 / Alerts","visible":true,"source_id":"","metric":"","value_path":"","value_expression":"","time_window":{"from":"-1h","to":"now"},"refresh_ms":60000,"thresholds":[],"style":{"unit":"","variant":"default","precision":2,"line_width":1,"show_legend":true,"invert":false},"linked_panel_ids":["cpu_trend"],"actions":[],"text":{"content":"CPU 告警预览 / CPU alert preview","multiline":false,"max_lines":1}}
      ]},
      {"id":"details","title":"详情 / Details","visible":true,"grid_columns":24,"row_height":16,"panels":[
        {"id":"cpu_table","kind":"table","grid":{"x":0,"y":0,"w":24,"h":10},"title":"CPU 明细 / CPU details","visible":true,"source_id":"sample","metric":"server.cpu","value_path":"$.value","value_expression":"","time_window":{"from":"-1h","to":"now"},"refresh_ms":60000,"thresholds":[],"style":{"unit":"percent","variant":"default","precision":1,"line_width":1,"show_legend":true,"invert":false},"linked_panel_ids":[],"actions":[],"columns":[{"key":"value","title":"数值 / Value","unit":"percent","width":6},{"key":"server","title":"服务器 / Server","unit":"","width":10}]},
        {"id":"back","kind":"button","grid":{"x":0,"y":10,"w":8,"h":4},"title":"返回 / Back","visible":true,"source_id":"","metric":"","value_path":"","value_expression":"","time_window":{"from":"-1h","to":"now"},"refresh_ms":60000,"thresholds":[],"style":{"unit":"","variant":"default","precision":2,"line_width":1,"show_legend":true,"invert":false},"linked_panel_ids":[],"actions":[{"id":"back","kind":"back","label":"返回 / Back","target":"","value":"","confirm":false}]}
      ]}
    ],
    "alert_rules":[{"id":"cpu_high","title":"CPU 过高 / High CPU","enabled":true,"source_id":"sample","metric":"server.cpu","expression":"value > 80","for_ms":0,"message":"服务器 ${server} CPU 过高","style":{"surfaces":["banner","card","table_row","sparkline"],"severity":"warning","linked_panel_id":"cpu_trend"}}],
    "assets":[],
    "samples":[
      {"source_id":"sample","metric":"token.used","timestamp_ms":1000,"value":640,"unit":"tokens","labels":{"account":"demo-account"}},
      {"source_id":"sample","metric":"server.cpu","timestamp_ms":1000,"value":42,"unit":"percent","labels":{"server":"srv-demo"}},
      {"source_id":"sample","metric":"server.cpu","timestamp_ms":2000,"value":86,"unit":"percent","labels":{"server":"srv-demo"}},
      {"source_id":"sample","metric":"server.cpu","timestamp_ms":3000,"value":67,"unit":"percent","labels":{"server":"srv-demo"}}
    ],
    "events":[{"id":"evt-sample","rule_id":"cpu_high","source_id":"sample","timestamp_ms":2000,"severity":"warning","state":"active","title":"CPU 过高 / High CPU","message":"srv-demo CPU 86%","linked_panel_id":"cpu_trend"}]
  },
  "source_plans":[{"id":"sample","kind":"static","enabled":true,"url":"","refresh_ms":60000,"timeout_ms":10000,"max_response_bytes":65536,"tls_verify":true,"auth_token_ref":"","headers":{},"response":"standard_json","method":"GET","body":"","static_body":"{\"metrics\":[{\"metric\":\"token.used\",\"value\":640,\"unit\":\"tokens\",\"timestamp_ms\":1000,\"labels\":{\"account\":\"demo-account\"}},{\"metric\":\"server.cpu\",\"value\":42,\"unit\":\"percent\",\"timestamp_ms\":1000,\"labels\":{\"server\":\"srv-demo\"}}],\"events\":[{\"id\":\"evt-sample\",\"rule_id\":\"cpu_high\",\"source_id\":\"sample\",\"timestamp_ms\":2000,\"severity\":\"warning\",\"state\":\"active\",\"title\":\"CPU high\",\"message\":\"srv-demo CPU 86%\",\"linked_panel_id\":\"cpu_trend\"}]}","text_path":"","reset_at_path":"$.reset_at","stale_after_ms":300000,"conditional_get":true,"max_json_depth":16}]
})JSON";
}

}  // namespace eego::admin
