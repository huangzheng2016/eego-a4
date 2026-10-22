// SPDX-License-Identifier: GPL-3.0-or-later
#include "dashboard_api.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "admin/admin_commands.h"
#include "admin/admin_log.h"
#include "admin/dashboard_config.h"
#include "dashboard_core/model.h"
#include "data_pipeline/net_stack_transport.h"
#include "data_pipeline/parser.h"
#include "esp_timer.h"

namespace eego::admin::internal {
namespace {

using dashboard::JsonValue;
using dashboard::ValidationError;
using dashboard::pipeline::HttpResponse;
using dashboard::pipeline::SourcePlan;
using dashboard::pipeline::SourceResult;

const char* statusText(int status) {
  switch (status) {
    case 200: return "200 OK";
    case 201: return "201 Created";
    case 204: return "204 No Content";
    case 400: return "400 Bad Request";
    case 404: return "404 Not Found";
    case 409: return "409 Conflict";
    case 413: return "413 Payload Too Large";
    case 415: return "415 Unsupported Media Type";
    case 500: return "500 Internal Server Error";
    case 503: return "503 Service Unavailable";
    default: return "500 Internal Server Error";
  }
}

esp_err_t sendJson(httpd_req_t* req, int status, const std::string& body) {
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_set_status(req, statusText(status));
  httpd_resp_send(req, body.c_str(), body.size());
  return ESP_OK;
}

JsonValue errorsToJson(const std::vector<ValidationError>& errors) {
  JsonValue::Object fields;
  for (const auto& error : errors) {
    fields.emplace_back(error.field, JsonValue::makeString(error.message));
  }
  return JsonValue::makeObject(std::move(fields));
}

esp_err_t sendError(httpd_req_t* req, int status, const char* code,
                    const std::string& message,
                    const std::vector<ValidationError>* errors = nullptr) {
  JsonValue::Object error;
  error.emplace_back("code", JsonValue::makeString(code));
  error.emplace_back("message", JsonValue::makeString(message));
  if (errors != nullptr && !errors->empty()) error.emplace_back("fields", errorsToJson(*errors));
  return sendJson(req, status,
                  JsonValue::makeObject({{"error", JsonValue::makeObject(std::move(error))}})
                      .toString());
}

esp_err_t sendDocument(httpd_req_t* req, const DashboardDocument& document, int status = 200,
                       bool dashboard_only = false) {
  return sendJson(req, status, DashboardStore::toJson(document, true, dashboard_only).toString());
}

bool contentTypeJson(httpd_req_t* req) {
  char value[128];
  if (httpd_req_get_hdr_value_str(req, "Content-Type", value, sizeof(value)) != ESP_OK) return false;
  const std::string type(value);
  return type.compare(0, 16, "application/json") == 0 &&
         (type.size() == 16 || type[16] == ';' || type[16] == ' ' || type[16] == '\t');
}

bool readBody(httpd_req_t* req, std::string& body) {
  if (!contentTypeJson(req)) {
    sendError(req, 415, "unsupported_media_type", "JSON content type required");
    return false;
  }
  char length[24];
  if (httpd_req_get_hdr_value_str(req, "Content-Length", length, sizeof(length)) == ESP_OK) {
    const long value = std::strtol(length, nullptr, 10);
    if (value > static_cast<long>(kDashboardBundleMaxBytes)) {
      sendError(req, 413, "payload_too_large", "dashboard body exceeds 192 KiB");
      return false;
    }
  }
  body.clear();
  char buffer[1024];
  int count = 0;
  while ((count = httpd_req_recv(req, buffer, sizeof(buffer))) > 0) {
    body.append(buffer, static_cast<size_t>(count));
    if (body.size() > kDashboardBundleMaxBytes) {
      sendError(req, 413, "payload_too_large", "dashboard body exceeds 192 KiB");
      return false;
    }
  }
  if (count < 0) {
    sendError(req, 400, "invalid_request", "incomplete request body");
    return false;
  }
  return true;
}

bool readJson(httpd_req_t* req, JsonValue& value) {
  std::string body;
  if (!readBody(req, body)) return false;
  dashboard::JsonParseError error;
  if (!dashboard::jsonParse(body, value, error, 18)) {
    sendError(req, 400, "invalid_request",
              "invalid JSON at byte " + std::to_string(error.offset) + ": " + error.message);
    return false;
  }
  return true;
}

struct Revision {
  bool check = false;
  uint64_t value = 0;
};

bool requestRevision(httpd_req_t* req, Revision& revision) {
  char value[64];
  if (httpd_req_get_hdr_value_str(req, "If-Match", value, sizeof(value)) != ESP_OK) return true;
  std::string text(value);
  if (text == "*") return true;
  if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
    text = text.substr(1, text.size() - 2);
  }
  if (text.empty() || text.size() > 20) {
    sendError(req, 400, "invalid_request", "If-Match must be a decimal dashboard revision or *");
    return false;
  }
  for (char c : text) {
    if (c < '0' || c > '9') {
      sendError(req, 400, "invalid_request", "If-Match must be a decimal dashboard revision or *");
      return false;
    }
  }
  errno = 0;
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
  if (errno != 0 || end != text.c_str() + text.size()) {
    sendError(req, 400, "invalid_request", "invalid dashboard revision");
    return false;
  }
  revision.check = true;
  revision.value = static_cast<uint64_t>(parsed);
  return true;
}

std::vector<std::string> segments(const char* path) {
  std::vector<std::string> out;
  std::string current;
  for (const char* p = path; *p != '\0'; ++p) {
    if (*p == '/') {
      if (!current.empty()) out.push_back(current);
      current.clear();
    } else {
      current.push_back(*p);
    }
  }
  if (!current.empty()) out.push_back(current);
  return out;
}

JsonValue* dashboardValue(JsonValue& root) {
  return root.find("dashboard");
}

JsonValue* collection(JsonValue& parent, const std::string& name) {
  JsonValue* value = parent.find(name);
  return value != nullptr && value->type() == JsonValue::Type::Array ? value : nullptr;
}

JsonValue* byId(JsonValue& array, const std::string& key, const std::string& id) {
  if (array.type() != JsonValue::Type::Array) return nullptr;
  for (auto& item : array.arrayItems()) {
    const JsonValue* value = item.find(key);
    if (value != nullptr && value->type() == JsonValue::Type::String && value->stringValue() == id)
      return &item;
  }
  return nullptr;
}

const JsonValue* byId(const JsonValue& array, const std::string& key, const std::string& id) {
  if (array.type() != JsonValue::Type::Array) return nullptr;
  for (const auto& item : array.arrayItems()) {
    const JsonValue* value = item.find(key);
    if (value != nullptr && value->type() == JsonValue::Type::String && value->stringValue() == id)
      return &item;
  }
  return nullptr;
}

std::string idValue(const JsonValue& object, const std::string& key) {
  const JsonValue* value = object.find(key);
  return value != nullptr && value->type() == JsonValue::Type::String ? value->stringValue() : "";
}

void set(JsonValue& object, const std::string& key, JsonValue value) {
  if (JsonValue* current = object.find(key)) {
    *current = std::move(value);
  } else {
    object.objectItems().emplace_back(key, std::move(value));
  }
}

bool eraseId(JsonValue& array, const std::string& key, const std::string& id) {
  if (array.type() != JsonValue::Type::Array) return false;
  auto& items = array.arrayItems();
  for (auto it = items.begin(); it != items.end(); ++it) {
    if (idValue(*it, key) == id) {
      items.erase(it);
      return true;
    }
  }
  return false;
}

esp_err_t commit(httpd_req_t* req, JsonValue& root, const DashboardDocument& current,
                 const Revision& revision, int success_status = 200) {
  DashboardParseResult parsed = DashboardStore::parse(root.toString(), &current);
  if (!parsed.ok()) {
    return sendError(req, 400, "validation_failed", "dashboard validation failed", &parsed.errors);
  }
  AdminCommand command;
  command.type = AdminCommandType::ApplyDashboard;
  command.dashboard = std::make_shared<DashboardDocument>(std::move(parsed.document));
  command.dashboard_check_revision = revision.check;
  command.dashboard_expected_revision = revision.value;
  AdminResult result;
  if (!enqueueAdminCommand(command, result, 10000)) {
    return sendError(req, 503, "unavailable", "dashboard command queue unavailable");
  }
  if (!result.ok) {
    if (result.conflict) return sendError(req, 409, "conflict", result.err);
    return sendError(req, 500, "internal_error",
                     result.err.empty() ? "dashboard commit failed" : result.err);
  }
  auto latest = DashboardStore::instance().snapshot();
  return sendDocument(req, *latest, success_status);
}

esp_err_t collectionResponse(httpd_req_t* req, const JsonValue& items, uint64_t revision) {
  return sendJson(req, 200,
                  JsonValue::makeObject({
                      {"revision", JsonValue::makeInt(static_cast<int64_t>(revision))},
                      {"items", items},
                  }).toString());
}

esp_err_t crud(httpd_req_t* req, JsonValue& root, JsonValue& parent,
               const std::string& collection_name, const std::string& id_key,
               const std::string& id, const DashboardDocument& current,
               const Revision& revision) {
  JsonValue* items = collection(parent, collection_name);
  if (items == nullptr) return sendError(req, 500, "internal_error", "invalid stored collection");
  if (req->method == HTTP_GET) {
    if (id.empty()) return collectionResponse(req, *items, current.revision);
    const JsonValue* item = byId(*items, id_key, id);
    return item == nullptr ? sendError(req, 404, "not_found", "resource not found")
                           : sendJson(req, 200, item->toString());
  }
  if (req->method == HTTP_POST && !id.empty()) {
    return sendError(req, 404, "not_found", "resource not found");
  }
  if ((req->method == HTTP_PUT || req->method == HTTP_DELETE) && id.empty()) {
    return sendError(req, 404, "not_found", "resource not found");
  }
  if (req->method == HTTP_DELETE) {
    if (!eraseId(*items, id_key, id)) return sendError(req, 404, "not_found", "resource not found");
    return commit(req, root, current, revision);
  }
  if (req->method != HTTP_POST && req->method != HTTP_PUT) {
    return sendError(req, 404, "not_found", "resource not found");
  }
  JsonValue resource;
  if (!readJson(req, resource)) return ESP_OK;
  if (resource.type() != JsonValue::Type::Object) {
    return sendError(req, 400, "invalid_request", "resource must be an object");
  }
  if (req->method == HTTP_PUT && idValue(resource, id_key) != id) {
    return sendError(req, 400, "validation_failed", "resource ID must match the path");
  }
  if (req->method == HTTP_POST) {
    items->arrayItems().push_back(std::move(resource));
    return commit(req, root, current, revision, 201);
  }
  JsonValue* existing = byId(*items, id_key, id);
  if (existing == nullptr) return sendError(req, 404, "not_found", "resource not found");
  *existing = std::move(resource);
  return commit(req, root, current, revision);
}

JsonValue* findPage(JsonValue& root, const std::string& id) {
  JsonValue* dash = dashboardValue(root);
  if (dash == nullptr) return nullptr;
  JsonValue* pages = collection(*dash, "pages");
  return pages == nullptr ? nullptr : byId(*pages, "id", id);
}

JsonValue* findPanel(JsonValue& page, const std::string& id) {
  JsonValue* panels = collection(page, "panels");
  return panels == nullptr ? nullptr : byId(*panels, "id", id);
}

void syncPlanWithSource(JsonValue& plan, const JsonValue& source) {
  static const char* kFields[] = {"id", "kind", "enabled", "url", "refresh_ms", "timeout_ms",
                                  "max_response_bytes", "tls_verify", "auth_token_ref", "headers",
                                  "header_secret_refs"};
  for (const char* field : kFields) {
    if (const JsonValue* value = source.find(field)) set(plan, field, *value);
  }
}

esp_err_t sourceCrud(httpd_req_t* req, JsonValue& root, const std::string& id,
                     const DashboardDocument& current, const Revision& revision) {
  JsonValue* dash = dashboardValue(root);
  JsonValue* sources = dash == nullptr ? nullptr : collection(*dash, "sources");
  JsonValue* plans = collection(root, "source_plans");
  if (sources == nullptr || plans == nullptr) {
    return sendError(req, 500, "internal_error", "invalid stored source collections");
  }
  if (req->method == HTTP_GET) {
    if (id.empty()) return collectionResponse(req, *sources, current.revision);
    const JsonValue* source = byId(*sources, "id", id);
    if (source == nullptr) return sendError(req, 404, "not_found", "source not found");
    if (const JsonValue* plan = byId(*plans, "id", id)) {
      return sendJson(req, 200,
                      JsonValue::makeObject({{"source", *source}, {"plan", *plan}}).toString());
    }
    return sendJson(req, 200, source->toString());
  }
  if (req->method == HTTP_DELETE && !id.empty()) {
    if (!eraseId(*sources, "id", id)) return sendError(req, 404, "not_found", "source not found");
    eraseId(*plans, "id", id);
    return commit(req, root, current, revision);
  }
  if ((req->method == HTTP_POST && !id.empty()) ||
      ((req->method == HTTP_PUT || req->method == HTTP_DELETE) && id.empty()) ||
      (req->method != HTTP_POST && req->method != HTTP_PUT && req->method != HTTP_DELETE)) {
    return sendError(req, 404, "not_found", "resource not found");
  }
  JsonValue body;
  if (!readJson(req, body)) return ESP_OK;
  JsonValue source = body;
  JsonValue plan;
  bool has_plan = false;
  bool remove_plan = false;
  if (body.type() == JsonValue::Type::Object && body.find("source") != nullptr) {
    const JsonValue* wrapped = body.find("source");
    if (wrapped->type() != JsonValue::Type::Object) {
      return sendError(req, 400, "invalid_request", "source must be an object");
    }
    source = *wrapped;
    if (const JsonValue* value = body.find("plan")) {
      has_plan = true;
      remove_plan = value->isNull();
      if (!remove_plan) {
        if (value->type() != JsonValue::Type::Object) {
          return sendError(req, 400, "invalid_request", "plan must be an object or null");
        }
        plan = *value;
      }
    }
  }
  if (source.type() != JsonValue::Type::Object) {
    return sendError(req, 400, "invalid_request", "source must be an object");
  }
  const std::string source_id = idValue(source, "id");
  if (req->method == HTTP_PUT && source_id != id) {
    return sendError(req, 400, "validation_failed", "source ID must match the path");
  }
  if (has_plan && !remove_plan) syncPlanWithSource(plan, source);

  if (req->method == HTTP_POST) {
    sources->arrayItems().push_back(std::move(source));
    if (has_plan && !remove_plan) plans->arrayItems().push_back(std::move(plan));
    return commit(req, root, current, revision, 201);
  }
  JsonValue* existing = byId(*sources, "id", id);
  if (existing == nullptr) return sendError(req, 404, "not_found", "source not found");
  *existing = std::move(source);
  JsonValue* existing_plan = byId(*plans, "id", id);
  if (has_plan) {
    if (remove_plan) {
      eraseId(*plans, "id", id);
    } else if (existing_plan != nullptr) {
      *existing_plan = std::move(plan);
    } else {
      plans->arrayItems().push_back(std::move(plan));
    }
  } else if (existing_plan != nullptr) {
    syncPlanWithSource(*existing_plan, *existing);
  }
  return commit(req, root, current, revision);
}

esp_err_t layout(httpd_req_t* req, JsonValue& root, JsonValue& page,
                 const DashboardDocument& current, const Revision& revision) {
  if (req->method == HTTP_GET) {
    JsonValue::Array panels;
    if (JsonValue* items = collection(page, "panels")) {
      for (const auto& panel : items->arrayItems()) {
        const JsonValue* grid = panel.find("grid");
        panels.push_back(JsonValue::makeObject({
            {"id", JsonValue::makeString(idValue(panel, "id"))},
            {"grid", grid == nullptr ? JsonValue::makeObject({}) : *grid},
        }));
      }
    }
    const JsonValue* columns = page.find("grid_columns");
    const JsonValue* height = page.find("row_height");
    return sendJson(req, 200,
                    JsonValue::makeObject({
                        {"page_id", JsonValue::makeString(idValue(page, "id"))},
                        {"grid_columns", columns == nullptr ? JsonValue::makeInt(24) : *columns},
                        {"row_height", height == nullptr ? JsonValue::makeInt(16) : *height},
                        {"panels", JsonValue::makeArray(std::move(panels))},
                    }).toString());
  }
  if (req->method == HTTP_DELETE) {
    set(page, "grid_columns", JsonValue::makeInt(24));
    set(page, "row_height", JsonValue::makeInt(16));
    if (JsonValue* panels = collection(page, "panels")) {
      for (auto& panel : panels->arrayItems()) {
        set(panel, "grid", JsonValue::makeObject({
                               {"x", JsonValue::makeInt(0)}, {"y", JsonValue::makeInt(0)},
                               {"w", JsonValue::makeInt(6)}, {"h", JsonValue::makeInt(4)},
                           }));
      }
    }
    return commit(req, root, current, revision);
  }
  if (req->method != HTTP_POST && req->method != HTTP_PUT) {
    return sendError(req, 404, "not_found", "resource not found");
  }
  JsonValue body;
  if (!readJson(req, body)) return ESP_OK;
  if (body.type() != JsonValue::Type::Object) {
    return sendError(req, 400, "invalid_request", "layout must be an object");
  }
  for (const auto& member : body.objectItems()) {
    if (member.first != "page_id" && member.first != "grid_columns" &&
        member.first != "row_height" && member.first != "panels") {
      return sendError(req, 400, "validation_failed", "unknown layout field: " + member.first);
    }
  }
  if (!idValue(body, "page_id").empty() && idValue(body, "page_id") != idValue(page, "id")) {
    return sendError(req, 400, "validation_failed", "layout page_id must match the path");
  }
  if (const JsonValue* value = body.find("grid_columns")) set(page, "grid_columns", *value);
  if (const JsonValue* value = body.find("row_height")) set(page, "row_height", *value);
  if (const JsonValue* panel_layouts = body.find("panels")) {
    if (panel_layouts->type() != JsonValue::Type::Array) {
      return sendError(req, 400, "validation_failed", "layout panels must be an array");
    }
    for (const auto& item : panel_layouts->arrayItems()) {
      if (item.type() != JsonValue::Type::Object) {
        return sendError(req, 400, "validation_failed", "panel layout must be an object");
      }
      for (const auto& member : item.objectItems()) {
        if (member.first != "id" && member.first != "grid") {
          return sendError(req, 400, "validation_failed", "unknown panel layout field");
        }
      }
      JsonValue* panel = findPanel(page, idValue(item, "id"));
      const JsonValue* grid = item.find("grid");
      if (panel == nullptr || grid == nullptr) {
        return sendError(req, 400, "validation_failed", "panel layout requires a known id and grid");
      }
      set(*panel, "grid", *grid);
    }
  }
  return commit(req, root, current, revision);
}

std::string previewText(const std::string& input, size_t maximum) {
  std::string out = input.substr(0, maximum);
  while (!out.empty() && !dashboard::jsonIsValidUtf8(out)) out.pop_back();
  return out;
}

bool previewSensitive(const std::string& key) {
  std::string lowered = key;
  for (char& c : lowered) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return lowered.find("token") != std::string::npos || lowered.find("secret") != std::string::npos ||
         lowered.find("pass") != std::string::npos || lowered.find("key") != std::string::npos ||
         lowered.find("auth") != std::string::npos || lowered.find("cookie") != std::string::npos;
}

JsonValue previewJson(const SourceResult& result, int status, int64_t latency_ms,
                      const std::string& error) {
  JsonValue::Array metrics;
  constexpr size_t kMetrics = 16;
  for (size_t i = 0; i < result.metrics.size() && i < kMetrics; ++i) {
    const auto& sample = result.metrics[i];
    JsonValue::Object labels;
    for (const auto& label : sample.labels) {
      labels.emplace_back(label.key,
                          JsonValue::makeString(previewSensitive(label.key)
                                                    ? "********"
                                                    : previewText(label.value, 128)));
    }
    metrics.push_back(JsonValue::makeObject({
        {"source_id", JsonValue::makeString(sample.source_id)},
        {"metric", JsonValue::makeString(previewText(sample.metric, 128))},
        {"timestamp_ms", JsonValue::makeInt(sample.timestamp_ms)},
        {"value", JsonValue::makeDouble(sample.value)},
        {"unit", JsonValue::makeString(previewText(sample.unit, 32))},
        {"labels", JsonValue::makeObject(std::move(labels))},
    }));
  }
  JsonValue::Array events;
  constexpr size_t kEvents = 8;
  for (size_t i = 0; i < result.events.size() && i < kEvents; ++i) {
    const auto& event = result.events[i];
    events.push_back(JsonValue::makeObject({
        {"id", JsonValue::makeString(previewText(event.id, 64))},
        {"rule_id", JsonValue::makeString(previewText(event.rule_id, 64))},
        {"source_id", JsonValue::makeString(previewText(event.source_id, 64))},
        {"timestamp_ms", JsonValue::makeInt(event.timestamp_ms)},
        {"severity", JsonValue::makeString(dashboard::severityName(event.severity))},
        {"state", JsonValue::makeString(dashboard::eventStateName(event.state))},
        {"title", JsonValue::makeString(previewText(event.title, 160))},
        {"message", JsonValue::makeString(previewText(event.message, 256))},
        {"linked_panel_id", JsonValue::makeString(previewText(event.linked_panel_id, 64))},
    }));
  }
  JsonValue::Array assets;
  if (result.has_image) {
    assets.push_back(JsonValue::makeObject({
        {"kind", JsonValue::makeString("image")},
        {"media_type", JsonValue::makeString(previewText(result.image.media_type, 64))},
        {"size_bytes", JsonValue::makeInt(static_cast<int64_t>(result.image.bytes.size()))},
        {"fetched_at_ms", JsonValue::makeInt(result.image.fetched_at_ms)},
    }));
  }
  JsonValue::Object out;
  out.emplace_back("source_id", JsonValue::makeString(result.source_id));
  out.emplace_back("status_code", JsonValue::makeInt(status));
  out.emplace_back("latency_ms", JsonValue::makeInt(latency_ms));
  if (!error.empty()) out.emplace_back("error", JsonValue::makeString(error));
  out.emplace_back("metrics", JsonValue::makeArray(std::move(metrics)));
  out.emplace_back("events", JsonValue::makeArray(std::move(events)));
  out.emplace_back("assets", JsonValue::makeArray(std::move(assets)));
  out.emplace_back("truncated", JsonValue::makeObject({
                                      {"metrics", JsonValue::makeBool(result.metrics.size() > kMetrics)},
                                      {"events", JsonValue::makeBool(result.events.size() > kEvents)},
                                  }));
  return JsonValue::makeObject(std::move(out));
}

const char* storedHeader(const DashboardDocument& current, const std::string& source_id,
                         const std::string& name) {
  for (const auto& plan : current.plans) {
    if (plan.source.id != source_id) continue;
    for (const auto& header : plan.source.headers) {
      if (header.key == name) return header.value.c_str();
    }
  }
  for (const auto& source : current.dashboard.sources) {
    if (source.id != source_id) continue;
    for (const auto& header : source.headers) {
      if (header.key == name) return header.value.c_str();
    }
  }
  return nullptr;
}

bool testPlan(httpd_req_t* req, const JsonValue& body, const DashboardDocument& current,
              SourcePlan& plan, dashboard::VariableValues& variables) {
  JsonValue value;
  if (const JsonValue* inline_plan = body.find("plan")) {
    value = *inline_plan;
  } else if (const JsonValue* id = body.find("id");
             id != nullptr && id->type() == JsonValue::Type::String) {
    if (const JsonValue* found = byId(current.source_plan_values, "id", id->stringValue())) {
      value = *found;
    } else {
      const JsonValue raw = dashboard::dashboardToJson(current.dashboard);
      const JsonValue* sources = raw.find("sources");
      const JsonValue* source = sources == nullptr ? nullptr : byId(*sources, "id", id->stringValue());
      if (source == nullptr) {
        sendError(req, 404, "not_found", "source not found");
        return false;
      }
      value = *source;
      set(value, "response", JsonValue::makeString("standard_json"));
    }
  } else if (const JsonValue* source = body.find("source")) {
    value = *source;
    set(value, "response", JsonValue::makeString("standard_json"));
  } else {
    sendError(req, 400, "invalid_request", "plan, source, or configured source id required");
    return false;
  }
  std::string error;
  if (!dashboard::pipeline::sourcePlanFromJson(value, plan, error)) {
    sendError(req, 400, "validation_failed", "source plan validation failed: " + error);
    return false;
  }
  for (auto& header : plan.source.headers) {
    if (header.value != "********") continue;
    const char* stored = storedHeader(current, plan.source.id, header.key);
    if (stored == nullptr) {
      sendError(req, 400, "validation_failed", "masked test header has no stored value");
      return false;
    }
    header.value = stored;
  }

  dashboard::VariableValues overrides;
  if (const JsonValue* values = body.find("variables")) {
    if (values->type() != JsonValue::Type::Object || values->objectItems().size() > 24) {
      sendError(req, 400, "validation_failed", "variables must be a bounded string map");
      return false;
    }
    for (const auto& variable : values->objectItems()) {
      if (variable.second.type() != JsonValue::Type::String) {
        sendError(req, 400, "validation_failed", "variable values must be strings");
        return false;
      }
      overrides.emplace_back(variable.first, variable.second.stringValue());
    }
  }
  if (current.configured &&
      !dashboard::defaultVariableValues(current.dashboard.variables, {}, variables, error)) {
    sendError(req, 400, "validation_failed", "variable validation failed: " + error);
    return false;
  }
  // Test drafts may introduce variables before the dashboard is applied. Merge
  // their values over device defaults instead of rejecting them as unknown.
  for (const auto& override_value : overrides) {
    if (dashboard::jsonCodePointCount(override_value.second) >
        dashboard::kMaxVariableValueCodePoints) {
      sendError(req, 400, "validation_failed", "variable value is invalid UTF-8 or too long");
      return false;
    }
    bool replaced = false;
    for (auto& value : variables) {
      if (value.first == override_value.first) {
        value.second = override_value.second;
        replaced = true;
        break;
      }
    }
    if (!replaced) variables.push_back(override_value);
  }
  return true;
}

esp_err_t sourceTest(httpd_req_t* req, const DashboardDocument& current) {
  JsonValue body;
  if (!readJson(req, body)) return ESP_OK;
  if (body.type() != JsonValue::Type::Object) {
    return sendError(req, 400, "invalid_request", "test request must be an object");
  }
  SourcePlan plan;
  dashboard::VariableValues variables;
  if (!testPlan(req, body, current, plan, variables)) return ESP_OK;

  const int64_t started = esp_timer_get_time();
  HttpResponse response;
  bool transported = true;
  if (plan.source.kind == dashboard::SourceKind::Static) {
    response.status = 200;
    response.body = plan.static_body;
    // No forced media type: JSON parsers do not need it, and image preview can
    // sniff PNG/JPEG without ever returning the static body.
  } else {
    net::NetRequest request;
    std::string error;
    if (!dashboard::pipeline::buildSourceRequest(
            plan, variables, dashboard::pipeline::makeNetStackTokenResolver(), request, error)) {
      return sendError(req, 400, "validation_failed", "request validation failed: " + error);
    }
    dashboard::pipeline::NetStackTransport transport(net::NetStack::instance(), 12000);
    transported = transport.execute(request, response);
    if (!transported && response.status == -1) {
      return sendError(req, 503, "unavailable", "net_task unavailable or timed out");
    }
  }
  const int64_t latency_ms = (esp_timer_get_time() - started) / 1000;
  SourceResult result;
  std::string parse_error;
  const int64_t now_ms = static_cast<int64_t>(std::time(nullptr)) * 1000;
  const bool parsed = dashboard::pipeline::parseSourceResponse(plan, response, variables, now_ms,
                                                               result, parse_error);
  if (!parsed) {
    result.source_id = plan.source.id;
    // Parser errors can contain mapped input identifiers; do not reflect them.
    const std::string safe_error = response.error.empty() ? "response normalization failed"
                                                          : previewText(response.error, 160);
    return sendJson(req, 200, previewJson(result, response.status, latency_ms, safe_error).toString());
  }
  ADMIN_LOG_INFO("dashboard source test: status=%d latency=%lldms", response.status,
                 static_cast<long long>(latency_ms));
  return sendJson(req, 200, previewJson(result, response.status, latency_ms, "").toString());
}

esp_err_t validateOnly(httpd_req_t* req, const DashboardDocument& current) {
  std::string body;
  if (!readBody(req, body)) return ESP_OK;
  DashboardParseResult parsed = DashboardStore::parse(body, &current);
  JsonValue::Array errors;
  for (const auto& error : parsed.errors) {
    errors.push_back(JsonValue::makeObject({
        {"field", JsonValue::makeString(error.field)},
        {"message", JsonValue::makeString(error.message)},
    }));
  }
  JsonValue::Object out;
  out.emplace_back("valid", JsonValue::makeBool(parsed.ok()));
  out.emplace_back("migrated", JsonValue::makeBool(parsed.migrated));
  out.emplace_back("source_version", JsonValue::makeInt(parsed.source_version));
  out.emplace_back("errors", JsonValue::makeArray(std::move(errors)));
  if (parsed.ok()) out.emplace_back("bundle", DashboardStore::toJson(parsed.document, true, false));
  return sendJson(req, 200, JsonValue::makeObject(std::move(out)).toString());
}

esp_err_t importOrReplace(httpd_req_t* req, const DashboardDocument& current, bool create_only) {
  if (create_only && current.configured) return sendError(req, 409, "conflict", "dashboard already exists");
  std::string body;
  if (!readBody(req, body)) return ESP_OK;
  Revision revision;
  if (!requestRevision(req, revision)) return ESP_OK;
  DashboardParseResult parsed = DashboardStore::parse(body, &current);
  if (!parsed.ok()) {
    return sendError(req, 400, "validation_failed", "dashboard import validation failed", &parsed.errors);
  }
  AdminCommand command;
  command.type = AdminCommandType::ApplyDashboard;
  command.dashboard = std::make_shared<DashboardDocument>(std::move(parsed.document));
  command.dashboard_check_revision = revision.check;
  command.dashboard_expected_revision = revision.value;
  AdminResult result;
  if (!enqueueAdminCommand(command, result, 10000)) {
    return sendError(req, 503, "unavailable", "dashboard command queue unavailable");
  }
  if (!result.ok) {
    if (result.conflict) return sendError(req, 409, "conflict", result.err);
    return sendError(req, 500, "internal_error",
                     result.err.empty() ? "dashboard commit failed" : result.err);
  }
  return sendDocument(req, *DashboardStore::instance().snapshot(), create_only ? 201 : 200);
}

esp_err_t eraseDashboard(httpd_req_t* req, const DashboardDocument& current) {
  if (!current.configured) return sendError(req, 404, "not_found", "dashboard is not configured");
  Revision revision;
  if (!requestRevision(req, revision)) return ESP_OK;
  AdminCommand command;
  command.type = AdminCommandType::DeleteDashboard;
  command.dashboard_check_revision = revision.check;
  command.dashboard_expected_revision = revision.value;
  AdminResult result;
  if (!enqueueAdminCommand(command, result, 10000)) {
    return sendError(req, 503, "unavailable", "dashboard command queue unavailable");
  }
  if (!result.ok) {
    if (result.conflict) return sendError(req, 409, "conflict", result.err);
    return sendError(req, 500, "internal_error",
                     result.err.empty() ? "dashboard delete failed" : result.err);
  }
  auto latest = DashboardStore::instance().snapshot();
  return sendJson(req, 200,
                  JsonValue::makeObject({
                      {"configured", JsonValue::makeBool(false)},
                      {"revision", JsonValue::makeInt(static_cast<int64_t>(latest->revision))},
                  }).toString());
}

const char* schemaInfo() {
  return R"JSON({
  "bundle":{"schema":"eego.dashboard-bundle/1","schema_version":1,"max_bytes":196608,"fields":["schema","schema_version","revision","dashboard","source_plans"]},
  "dashboard":{"schema":"eego.dashboard/2","schema_version":2,"accepted_versions":[1,2],"canonical_schema_file":"dashboard_core/schema/eego-dashboard-v2.schema.json"},
  "source_plan":{"max_bytes":32768,"max_items":12,"script_execution":false,"auth_token_ref":"nvs:source/<key>","headers":"non-secret literal values only","header_secret_refs":"header name -> nvs:source/<key>","responses":["generic_json","prometheus","standard_json","metrics_json","events_json","image","text"]},
  "resources":{
    "dashboard":"/api/v1/dashboard",
    "pages":"/api/v1/dashboard/pages",
    "panels":"/api/v1/dashboard/pages/{page}/panels",
    "actions":"/api/v1/dashboard/pages/{page}/panels/{panel}/actions",
    "sources":"/api/v1/dashboard/sources",
    "source_credentials":"/api/v1/sources/credentials",
    "variables":"/api/v1/dashboard/variables",
    "alerts":"/api/v1/dashboard/alerts",
    "layout":"/api/v1/dashboard/pages/{page}/layout"
  },
  "concurrency":"Send If-Match: <revision> on mutations; stale revisions return 409.",
  "security":"Bearer required; JSON writes enforce same-origin and content type. Credentials are write-only in NVS source; SD and exports carry refs only. Raw response bodies are never returned."
})JSON";
}

}  // namespace

esp_err_t handleDashboardApi(httpd_req_t* req, const char* path) {
  const std::vector<std::string> parts = segments(path);
  if (parts.empty() || parts[0] != "dashboard") {
    return sendError(req, 404, "not_found", "resource not found");
  }
  auto current = DashboardStore::instance().snapshot();

  if (parts.size() == 1) {
    if (req->method == HTTP_GET) {
      if (!current->configured) {
        return sendJson(req, 404,
                        JsonValue::makeObject({
                            {"error", JsonValue::makeObject({
                                          {"code", JsonValue::makeString("not_found")},
                                          {"message", JsonValue::makeString("dashboard is not configured")},
                                      })},
                            {"configured", JsonValue::makeBool(false)},
                            {"revision", JsonValue::makeInt(static_cast<int64_t>(current->revision))},
                        }).toString());
      }
      return sendDocument(req, *current);
    }
    if (req->method == HTTP_POST) return importOrReplace(req, *current, true);
    if (req->method == HTTP_PUT) return importOrReplace(req, *current, false);
    if (req->method == HTTP_DELETE) return eraseDashboard(req, *current);
    return sendError(req, 404, "not_found", "resource not found");
  }
  if (parts.size() == 2 && parts[1] == "validate" && req->method == HTTP_POST) {
    return validateOnly(req, *current);
  }
  if (parts.size() == 2 && parts[1] == "import" && req->method == HTTP_POST) {
    return importOrReplace(req, *current, false);
  }
  if (parts.size() == 2 && parts[1] == "sample" && req->method == HTTP_GET) {
    return sendJson(req, 200, DashboardStore::sampleJson());
  }
  if (parts.size() == 2 && parts[1] == "schema" && req->method == HTTP_GET) {
    return sendJson(req, 200, schemaInfo());
  }
  if (parts.size() == 2 && parts[1] == "export" && req->method == HTTP_GET) {
    if (!current->configured) return sendError(req, 404, "not_found", "dashboard is not configured");
    bool dashboard_only = false;
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
      char format[16];
      if (httpd_query_key_value(query, "format", format, sizeof(format)) == ESP_OK) {
        dashboard_only = std::strcmp(format, "dashboard") == 0 || std::strcmp(format, "canonical") == 0;
      }
    }
    return sendDocument(req, *current, 200, dashboard_only);
  }
  if (parts.size() == 3 && parts[1] == "sources" && parts[2] == "test" &&
      req->method == HTTP_POST) {
    return sourceTest(req, *current);
  }

  Revision revision;
  if (req->method != HTTP_GET && !requestRevision(req, revision)) return ESP_OK;
  const bool masked = req->method == HTTP_GET;
  JsonValue root = DashboardStore::toJson(*current, masked, false);

  if (parts[1] == "sources" && (parts.size() == 2 || parts.size() == 3)) {
    if (!current->configured) return sendError(req, 404, "not_found", "dashboard is not configured");
    return sourceCrud(req, root, parts.size() == 3 ? parts[2] : "", *current, revision);
  }
  if (parts[1] == "variables" && (parts.size() == 2 || parts.size() == 3)) {
    if (!current->configured) return sendError(req, 404, "not_found", "dashboard is not configured");
    JsonValue* dash = dashboardValue(root);
    return crud(req, root, *dash, "variables", "name", parts.size() == 3 ? parts[2] : "",
                *current, revision);
  }
  if (parts[1] == "alerts" && (parts.size() == 2 || parts.size() == 3)) {
    if (!current->configured) return sendError(req, 404, "not_found", "dashboard is not configured");
    JsonValue* dash = dashboardValue(root);
    return crud(req, root, *dash, "alert_rules", "id", parts.size() == 3 ? parts[2] : "",
                *current, revision);
  }
  if (parts[1] == "pages" && parts.size() >= 2) {
    if (!current->configured) return sendError(req, 404, "not_found", "dashboard is not configured");
    JsonValue* dash = dashboardValue(root);
    if (parts.size() == 2 || parts.size() == 3) {
      return crud(req, root, *dash, "pages", "id", parts.size() == 3 ? parts[2] : "",
                  *current, revision);
    }
    JsonValue* page = findPage(root, parts[2]);
    if (page == nullptr) return sendError(req, 404, "not_found", "page not found");
    if (parts.size() == 4 && parts[3] == "layout") {
      return layout(req, root, *page, *current, revision);
    }
    if (parts[3] == "panels" && (parts.size() == 4 || parts.size() == 5)) {
      return crud(req, root, *page, "panels", "id", parts.size() == 5 ? parts[4] : "",
                  *current, revision);
    }
    if (parts[3] == "panels" && parts.size() >= 6) {
      JsonValue* panel = findPanel(*page, parts[4]);
      if (panel == nullptr) return sendError(req, 404, "not_found", "panel not found");
      if (parts[5] == "actions" && (parts.size() == 6 || parts.size() == 7)) {
        return crud(req, root, *panel, "actions", "id", parts.size() == 7 ? parts[6] : "",
                    *current, revision);
      }
    }
  }
  return sendError(req, 404, "not_found", "resource not found");
}

}  // namespace eego::admin::internal
