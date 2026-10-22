// SPDX-License-Identifier: GPL-3.0-or-later
#include "data_pipeline/source_plan.h"

#include <algorithm>
#include <cctype>
#include <set>

#include "alert_core/sha1.h"
#include "dashboard_core/expression.h"
#include "dashboard_core/json.h"
#include "data_pipeline/json_path.h"
#include "net_stack/credentials.h"

namespace eego::dashboard::pipeline {
namespace {

bool validId(std::string_view id) {
  if (id.empty() || id.size() > 64) return false;
  return std::all_of(id.begin(), id.end(), [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
  });
}

bool validHeaderName(std::string_view name) {
  return net::CredentialStore::validHeaderName(std::string(name));
}

std::string lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

bool reservedHeader(std::string_view name) {
  const std::string key = lower(name);
  return key == "authorization" || key == "host" || key == "content-length";
}

bool safeHeaderValue(std::string_view value) {
  return std::all_of(value.begin(), value.end(), [](unsigned char c) {
    return c == '\t' || (c >= 0x20 && c != 0x7f);
  });
}

bool checkPath(std::string_view path, bool required, std::string& error) {
  if (path.empty()) {
    if (required) error = "required JSONPath is empty";
    return !required;
  }
  return validateJsonPath(path, error);
}

bool readString(const JsonValue& object, const char* key, std::string& out,
                bool required, std::string& error) {
  const JsonValue* value = object.find(key);
  if (value == nullptr) {
    if (required) error = std::string("missing string field: ") + key;
    return !required;
  }
  if (value->type() != JsonValue::Type::String || !jsonIsValidUtf8(value->stringValue())) {
    error = std::string("field must be a UTF-8 string: ") + key;
    return false;
  }
  out = value->stringValue();
  return true;
}

bool readBool(const JsonValue& object, const char* key, bool& out, std::string& error) {
  const JsonValue* value = object.find(key);
  if (value == nullptr) return true;
  if (value->type() != JsonValue::Type::Bool) {
    error = std::string("field must be boolean: ") + key;
    return false;
  }
  out = value->boolValue();
  return true;
}

bool readUint(const JsonValue& object, const char* key, uint32_t& out, uint32_t minimum,
              uint32_t maximum, std::string& error) {
  const JsonValue* value = object.find(key);
  if (value == nullptr) return true;
  if (value->type() != JsonValue::Type::Int || value->intValue() < minimum ||
      value->intValue() > maximum) {
    error = std::string("integer field out of range: ") + key;
    return false;
  }
  out = static_cast<uint32_t>(value->intValue());
  return true;
}

bool readSize(const JsonValue& object, const char* key, size_t& out, size_t minimum,
              size_t maximum, std::string& error) {
  uint32_t value = static_cast<uint32_t>(out);
  if (!readUint(object, key, value, static_cast<uint32_t>(minimum),
                static_cast<uint32_t>(maximum), error))
    return false;
  out = value;
  return true;
}

bool knownFields(const JsonValue& object, std::initializer_list<std::string_view> fields,
                 std::string& error) {
  if (object.type() != JsonValue::Type::Object) {
    error = "plan value must be an object";
    return false;
  }
  for (const auto& member : object.objectItems()) {
    if (std::find(fields.begin(), fields.end(), member.first) == fields.end()) {
      error = "unknown plan field: " + member.first;
      return false;
    }
  }
  return true;
}

bool parseHeaders(const JsonValue& value, std::vector<KeyValue>& out, bool secrets,
                  std::string& error) {
  if (value.type() != JsonValue::Type::Object || value.objectItems().size() > kMaxHeaders) {
    error = "headers must be a bounded object";
    return false;
  }
  out.clear();
  for (const auto& member : value.objectItems()) {
    if (!validHeaderName(member.first) || reservedHeader(member.first) ||
        member.second.type() != JsonValue::Type::String) {
      error = "invalid or reserved HTTP header";
      return false;
    }
    if (secrets) {
      if (!net::CredentialStore::validSourceRef(member.second.stringValue())) {
        error = "secret headers require nvs:source/<key> references";
        return false;
      }
    } else if (net::CredentialStore::sensitiveHeaderName(member.first) ||
               !safeHeaderValue(member.second.stringValue())) {
      error = "plain headers must contain only non-sensitive safe values";
      return false;
    }
    out.push_back({member.first, member.second.stringValue()});
  }
  return true;
}

bool parseLabels(const JsonValue& value, std::vector<LabelMapping>& out, std::string& error) {
  if (value.type() != JsonValue::Type::Array || value.arrayItems().size() > kMaxLabels) {
    error = "labels must be an array with at most 16 entries";
    return false;
  }
  out.clear();
  for (const JsonValue& item : value.arrayItems()) {
    LabelMapping label;
    if (!knownFields(item, {"name", "path", "value"}, error) ||
        !readString(item, "name", label.name, true, error) ||
        !readString(item, "path", label.path, false, error) ||
        !readString(item, "value", label.value, false, error)) {
      return false;
    }
    out.push_back(std::move(label));
  }
  return true;
}

bool parseFields(const JsonValue& value, std::vector<FieldMapping>& out, std::string& error) {
  if (value.type() != JsonValue::Type::Object || value.objectItems().size() > 32) {
    error = "fields must be a bounded object";
    return false;
  }
  out.clear();
  for (const auto& member : value.objectItems()) {
    if (member.first.empty() || member.second.type() != JsonValue::Type::String) {
      error = "field mappings must contain non-empty names and string paths";
      return false;
    }
    out.push_back({member.first, member.second.stringValue()});
  }
  return true;
}

bool parseMetric(const JsonValue& value, MetricMapping& out, std::string& error) {
  out = MetricMapping{};
  if (!knownFields(value,
                   {"name", "items_path", "name_path", "value_path", "timestamp_path",
                    "unit", "unit_path", "target_unit", "expression", "reset_at_path",
                    "fields", "labels"},
                   error) ||
      !readString(value, "name", out.name, false, error) ||
      !readString(value, "items_path", out.items_path, false, error) ||
      !readString(value, "name_path", out.name_path, false, error) ||
      !readString(value, "value_path", out.value_path, false, error) ||
      !readString(value, "timestamp_path", out.timestamp_path, false, error) ||
      !readString(value, "unit", out.unit, false, error) ||
      !readString(value, "unit_path", out.unit_path, false, error) ||
      !readString(value, "target_unit", out.target_unit, false, error) ||
      !readString(value, "expression", out.expression, false, error) ||
      !readString(value, "reset_at_path", out.reset_at_path, false, error)) {
    return false;
  }
  if (const JsonValue* fields = value.find("fields")) {
    if (!parseFields(*fields, out.fields, error)) return false;
  }
  if (const JsonValue* labels = value.find("labels")) {
    if (!parseLabels(*labels, out.labels, error)) return false;
  }
  return true;
}

bool parseEvent(const JsonValue& value, EventMapping& out, std::string& error) {
  out = EventMapping{};
  if (!knownFields(value,
                   {"items_path", "id_path", "rule_id_path", "timestamp_path",
                    "severity_path", "state_path", "title_path", "message_path",
                    "linked_panel_id_path", "severity_map", "clear_states"},
                   error) ||
      !readString(value, "items_path", out.items_path, false, error) ||
      !readString(value, "id_path", out.id_path, false, error) ||
      !readString(value, "rule_id_path", out.rule_id_path, false, error) ||
      !readString(value, "timestamp_path", out.timestamp_path, false, error) ||
      !readString(value, "severity_path", out.severity_path, false, error) ||
      !readString(value, "state_path", out.state_path, false, error) ||
      !readString(value, "title_path", out.title_path, false, error) ||
      !readString(value, "message_path", out.message_path, false, error) ||
      !readString(value, "linked_panel_id_path", out.linked_panel_id_path, false, error)) {
    return false;
  }
  if (const JsonValue* severity = value.find("severity_map")) {
    if (severity->type() != JsonValue::Type::Object || severity->objectItems().size() > 32) {
      error = "severity_map must be a bounded object";
      return false;
    }
    for (const auto& member : severity->objectItems()) {
      if (member.second.type() != JsonValue::Type::String) {
        error = "severity_map values must be strings";
        return false;
      }
      out.severity_map[member.first] = member.second.stringValue();
    }
  }
  if (const JsonValue* states = value.find("clear_states")) {
    if (states->type() != JsonValue::Type::Array || states->arrayItems().size() > 32) {
      error = "clear_states must be a bounded string array";
      return false;
    }
    for (const JsonValue& state : states->arrayItems()) {
      if (state.type() != JsonValue::Type::String) {
        error = "clear_states values must be strings";
        return false;
      }
      out.clear_states.push_back(state.stringValue());
    }
  }
  return true;
}

bool validateMetric(const MetricMapping& mapping, std::string& error) {
  if (mapping.name.empty() == mapping.name_path.empty()) {
    error = "metric mapping requires exactly one of name or name_path";
    return false;
  }
  if (!checkPath(mapping.items_path, true, error) ||
      !checkPath(mapping.value_path, true, error) ||
      !checkPath(mapping.name_path, false, error) ||
      !checkPath(mapping.timestamp_path, false, error) ||
      !checkPath(mapping.unit_path, false, error) ||
      !checkPath(mapping.reset_at_path, false, error)) {
    return false;
  }
  if (!jsonIsValidUtf8(mapping.name) || jsonCodePointCount(mapping.name) > 128 ||
      !jsonIsValidUtf8(mapping.unit) || jsonCodePointCount(mapping.unit) > 32 ||
      !jsonIsValidUtf8(mapping.target_unit) || jsonCodePointCount(mapping.target_unit) > 32) {
    error = "metric name or unit exceeds its budget";
    return false;
  }
  if (!mapping.expression.empty()) {
    ExpressionError expression_error;
    if (!validateExpressionSyntax(mapping.expression, expression_error)) {
      error = "invalid metric expression: " + expression_error.message;
      return false;
    }
  }
  std::set<std::string> names;
  for (const FieldMapping& field : mapping.fields) {
    if (field.name.empty() || !names.insert(field.name).second ||
        !checkPath(field.path, true, error)) {
      if (error.empty()) error = "invalid or duplicate expression field mapping";
      return false;
    }
  }
  names.clear();
  for (const LabelMapping& label : mapping.labels) {
    if (label.name.empty() || !jsonIsValidUtf8(label.name) ||
        jsonCodePointCount(label.name) > 64 || !names.insert(label.name).second ||
        label.path.empty() == label.value.empty() || !checkPath(label.path, false, error)) {
      if (error.empty()) error = "invalid or duplicate label mapping";
      return false;
    }
  }
  return true;
}

bool validateEvent(const EventMapping& mapping, std::string& error) {
  if (!checkPath(mapping.items_path, true, error) ||
      !checkPath(mapping.id_path, false, error) ||
      !checkPath(mapping.rule_id_path, false, error) ||
      !checkPath(mapping.timestamp_path, false, error) ||
      !checkPath(mapping.severity_path, false, error) ||
      !checkPath(mapping.state_path, false, error) ||
      !checkPath(mapping.title_path, false, error) ||
      !checkPath(mapping.message_path, false, error) ||
      !checkPath(mapping.linked_panel_id_path, false, error)) {
    return false;
  }
  for (const auto& severity : mapping.severity_map) {
    Severity parsed = Severity::Info;
    if (!severityFromString(severity.second, parsed)) {
      error = "severity_map contains a non-canonical target";
      return false;
    }
  }
  return true;
}

SourcePlan basePlan(const std::string& id, const std::string& url) {
  SourcePlan plan;
  plan.source.id = id;
  plan.source.url = url;
  return plan;
}

void appendKeyPart(std::string& key, std::string_view value) {
  key += std::to_string(value.size());
  key.push_back(':');
  key.append(value);
  key.push_back(';');
}

}  // namespace

bool validateSourcePlan(const SourcePlan& plan, std::string& error) {
  error.clear();
  const Source& source = plan.source;
  if (!validId(source.id)) {
    error = "source id must match [A-Za-z0-9_-]{1,64}";
    return false;
  }
  if (source.refresh_ms < 1000 || source.refresh_ms > 86400000 ||
      source.timeout_ms < 100 || source.timeout_ms > 120000 ||
      source.max_response_bytes < 1024 || source.max_response_bytes > 262144) {
    error = "source timing or response budget is out of range";
    return false;
  }
  if (plan.stale_after_ms > 604800000 || plan.max_json_depth == 0 || plan.max_json_depth > 16) {
    error = "source stale/depth budget is out of range";
    return false;
  }
  if (plan.method != "GET" && plan.method != "POST" && plan.method != "HEAD") {
    error = "only GET, POST, and HEAD requests are supported";
    return false;
  }
  if (source.kind == SourceKind::Http) {
    if (source.url.size() > kMaxUrlCodePoints ||
        source.url.find_first_of(" \t\r\n") != std::string::npos ||
        (source.url.rfind("http://", 0) != 0 && source.url.rfind("https://", 0) != 0)) {
      error = "HTTP source requires an http(s) URL";
      return false;
    }
  } else if (!source.url.empty()) {
    error = "static source URL must be empty";
    return false;
  }
  if (!source.auth_token_ref.empty() &&
      !net::CredentialStore::validSourceRef(source.auth_token_ref)) {
    error = "auth_token_ref must use nvs:source/<key>; re-create legacy refs via credentials API";
    return false;
  }
  if (source.headers.size() + source.header_secret_refs.size() > kMaxHeaders) {
    error = "too many HTTP headers";
    return false;
  }
  for (size_t i = 0; i < source.headers.size(); ++i) {
    const KeyValue& header = source.headers[i];
    if (!validHeaderName(header.key) || reservedHeader(header.key) ||
        net::CredentialStore::sensitiveHeaderName(header.key) || !safeHeaderValue(header.value)) {
      error = "plain headers must contain only non-sensitive safe values";
      return false;
    }
    for (size_t j = 0; j < i; ++j) {
      if (lower(header.key) == lower(source.headers[j].key)) {
        error = "duplicate HTTP header";
        return false;
      }
    }
  }
  for (size_t i = 0; i < source.header_secret_refs.size(); ++i) {
    const KeyValue& header = source.header_secret_refs[i];
    if (!validHeaderName(header.key) || reservedHeader(header.key) ||
        !net::CredentialStore::validSourceRef(header.value)) {
      error = "header_secret_refs requires safe names and nvs:source/<key> refs";
      return false;
    }
    for (size_t j = 0; j < i; ++j) {
      if (lower(header.key) == lower(source.header_secret_refs[j].key)) {
        error = "duplicate secret HTTP header";
        return false;
      }
    }
    for (const KeyValue& plain : source.headers) {
      if (lower(header.key) == lower(plain.key)) {
        error = "ambiguous HTTP header";
        return false;
      }
    }
  }
  if (!checkPath(plan.reset_at_path, false, error) ||
      !checkPath(plan.text_path, false, error)) {
    return false;
  }
  if (plan.metrics.size() > 64 || plan.events.size() > 16) {
    error = "too many metric or event mappings";
    return false;
  }
  for (const MetricMapping& metric : plan.metrics) {
    if (!validateMetric(metric, error)) return false;
  }
  for (const EventMapping& event : plan.events) {
    if (!validateEvent(event, error)) return false;
  }
  if (plan.response_kind == ResponseKind::GenericJson && plan.metrics.empty() &&
      plan.events.empty() && plan.text_path.empty()) {
    error = "generic_json requires a metric, event, or text mapping";
    return false;
  }
  return true;
}

bool sourcePlanFromJson(const JsonValue& value, SourcePlan& out, std::string& error) {
  out = SourcePlan{};
  if (!knownFields(value,
                   {"id", "kind", "enabled", "url", "refresh_ms", "timeout_ms",
                    "max_response_bytes", "tls_verify", "auth_token_ref", "headers",
                    "header_secret_refs", "response", "method", "body", "static_body", "metrics", "events",
                    "text_path", "reset_at_path", "stale_after_ms", "conditional_get",
                    "max_json_depth"},
                   error) ||
      !readString(value, "id", out.source.id, true, error) ||
      !readBool(value, "enabled", out.source.enabled, error) ||
      !readString(value, "url", out.source.url, false, error) ||
      !readUint(value, "refresh_ms", out.source.refresh_ms, 1000, 86400000, error) ||
      !readUint(value, "timeout_ms", out.source.timeout_ms, 100, 120000, error) ||
      !readUint(value, "max_response_bytes", out.source.max_response_bytes, 1024, 262144,
                error) ||
      !readBool(value, "tls_verify", out.source.tls_verify, error) ||
      !readString(value, "auth_token_ref", out.source.auth_token_ref, false, error) ||
      !readString(value, "method", out.method, false, error) ||
      !readString(value, "body", out.body, false, error) ||
      !readString(value, "static_body", out.static_body, false, error) ||
      !readString(value, "text_path", out.text_path, false, error) ||
      !readString(value, "reset_at_path", out.reset_at_path, false, error) ||
      !readUint(value, "stale_after_ms", out.stale_after_ms, 0, 604800000, error) ||
      !readBool(value, "conditional_get", out.conditional_get, error) ||
      !readSize(value, "max_json_depth", out.max_json_depth, 1, 16, error)) {
    return false;
  }
  if (value.find("kind") != nullptr) {
    std::string name;
    if (!readString(value, "kind", name, true, error) ||
        !sourceKindFromString(name, out.source.kind)) {
      if (error.empty()) error = "unsupported source kind";
      return false;
    }
  }
  if (value.find("response") != nullptr) {
    std::string name;
    if (!readString(value, "response", name, true, error)) return false;
    if (name == "standard") name = "standard_json";
    if (name == "metrics") name = "metrics_json";
    if (name == "events") name = "events_json";
    if (!responseKindFromString(name, out.response_kind)) {
      error = "unsupported response kind: " + name;
      return false;
    }
  }
  if (const JsonValue* headers = value.find("headers")) {
    if (!parseHeaders(*headers, out.source.headers, false, error)) return false;
  }
  if (const JsonValue* headers = value.find("header_secret_refs")) {
    if (!parseHeaders(*headers, out.source.header_secret_refs, true, error)) return false;
  }
  if (const JsonValue* metrics = value.find("metrics")) {
    if (metrics->type() != JsonValue::Type::Array || metrics->arrayItems().size() > 64) {
      error = "metrics must be a bounded mapping array";
      return false;
    }
    for (const JsonValue& item : metrics->arrayItems()) {
      MetricMapping mapping;
      if (!parseMetric(item, mapping, error)) return false;
      out.metrics.push_back(std::move(mapping));
    }
  }
  if (const JsonValue* events = value.find("events")) {
    if (events->type() != JsonValue::Type::Array || events->arrayItems().size() > 16) {
      error = "events must be a bounded mapping array";
      return false;
    }
    for (const JsonValue& item : events->arrayItems()) {
      EventMapping mapping;
      if (!parseEvent(item, mapping, error)) return false;
      out.events.push_back(std::move(mapping));
    }
  }
  return validateSourcePlan(out, error);
}

bool parseSourcePlan(std::string_view json, SourcePlan& out, std::string& error) {
  if (json.size() > 128 * 1024) {
    error = "source plan exceeds byte budget";
    return false;
  }
  JsonValue root;
  JsonParseError parse_error;
  if (!jsonParse(json, root, parse_error)) {
    error = "invalid source plan JSON at byte " + std::to_string(parse_error.offset) + ": " +
            parse_error.message;
    return false;
  }
  return sourcePlanFromJson(root, out, error);
}

bool buildSourceRequest(const SourcePlan& plan, const VariableValues& variables,
                        const TokenResolver& token_resolver, net::NetRequest& out,
                        std::string& error) {
  if (!validateSourcePlan(plan, error)) return false;
  if (plan.source.kind != SourceKind::Http) {
    error = "static sources do not have HTTP requests";
    return false;
  }
  out = net::NetRequest{};
  out.method = plan.method;
  out.timeout_ms = plan.source.timeout_ms;
  out.tls_verify = plan.source.tls_verify;
  out.conditional_get = plan.conditional_get;
  if (!interpolateVariables(plan.source.url, variables, out.url, error, 4096)) return false;
  if (out.url.find_first_of(" \t\r\n") != std::string::npos ||
      (out.url.rfind("http://", 0) != 0 && out.url.rfind("https://", 0) != 0)) {
    error = "interpolated URL must remain a valid http(s) URL";
    return false;
  }
  for (const KeyValue& header : plan.source.headers) {
    std::string value;
    if (!interpolateVariables(header.value, variables, value, error, 2048) ||
        !safeHeaderValue(value)) {
      if (error.empty()) error = "interpolated HTTP header contains a control character";
      return false;
    }
    out.headers.push_back({header.key, std::move(value)});
  }
  if (!interpolateVariables(plan.body, variables, out.body, error, 64 * 1024)) return false;
  const auto failCredential = [&]() {
    for (auto& header : out.headers) net::CredentialStore::secureErase(header.value);
    out.headers.clear();
    error = "configured source credential is unavailable or invalid";
    return false;
  };
  const auto resolve = [&](const std::string& ref, std::string& secret) {
    if (!token_resolver) return false;
    secret = token_resolver(ref);
    return net::CredentialStore::validSourceValue(secret);
  };
  if (!plan.source.auth_token_ref.empty()) {
    std::string token;
    if (!resolve(plan.source.auth_token_ref, token)) {
      net::CredentialStore::secureErase(token);
      return failCredential();
    }
    out.headers.push_back({"Authorization", "Bearer " + token});
    net::CredentialStore::secureErase(token);
  }
  for (const KeyValue& header : plan.source.header_secret_refs) {
    std::string secret;
    if (!resolve(header.value, secret)) {
      net::CredentialStore::secureErase(secret);
      return failCredential();
    }
    out.headers.push_back({header.key, std::move(secret)});
    net::CredentialStore::secureErase(secret);
  }
  return true;
}

std::string sourceRequestKey(const net::NetRequest& request) {
  std::string key;
  appendKeyPart(key, request.method);
  appendKeyPart(key, request.url);
  for (const net::NetHeader& header : request.headers) {
    appendKeyPart(key, header.name);
    appendKeyPart(key, alert_core::sha1Hex(header.value));
  }
  appendKeyPart(key, request.body);
  appendKeyPart(key, request.tls_verify ? "1" : "0");
  appendKeyPart(key, request.conditional_get ? "1" : "0");
  appendKeyPart(key, std::to_string(request.timeout_ms));
  return key;
}

SourcePlan makeTokenUsagePlan(const std::string& id, const std::string& url) {
  SourcePlan plan = basePlan(id, url);
  plan.response_kind = ResponseKind::GenericJson;
  MetricMapping metric;
  metric.items_path = "$.metrics[*]";
  metric.name_path = "$.metric";
  metric.value_path = "$.value";
  metric.timestamp_path = "$.timestamp_ms";
  metric.unit_path = "$.unit";
  metric.reset_at_path = "$.reset_at";
  plan.metrics.push_back(std::move(metric));
  return plan;
}

SourcePlan makeWeatherPlan(const std::string& id, const std::string& url) {
  SourcePlan plan = basePlan(id, url);
  plan.response_kind = ResponseKind::GenericJson;
  const struct {
    const char* name;
    const char* path;
    const char* unit;
  } kMappings[] = {
      {"weather.temperature", "$.temperature", "C"},
      {"weather.humidity", "$.humidity", "percent"},
      {"weather.wind_speed", "$.wind_speed", "km/h"},
  };
  for (const auto& item : kMappings) {
    MetricMapping metric;
    metric.name = item.name;
    metric.items_path = "$";
    metric.value_path = item.path;
    metric.unit = item.unit;
    metric.labels.push_back({"city", "$.city", ""});
    plan.metrics.push_back(std::move(metric));
  }
  return plan;
}

SourcePlan makeServerMetricsPlan(const std::string& id, const std::string& url) {
  SourcePlan plan = basePlan(id, url);
  plan.response_kind = ResponseKind::Prometheus;
  return plan;
}

SourcePlan makeSecurityEventsPlan(const std::string& id, const std::string& url) {
  SourcePlan plan = basePlan(id, url);
  plan.response_kind = ResponseKind::GenericJson;
  EventMapping events;
  events.severity_map = {{"error", "critical"}, {"warn", "warning"}, {"notice", "info"}};
  events.clear_states = {"cleared", "resolved", "inactive"};
  plan.events.push_back(std::move(events));
  return plan;
}

}  // namespace eego::dashboard::pipeline
