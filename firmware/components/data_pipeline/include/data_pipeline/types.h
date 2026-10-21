// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "dashboard_core/model.h"
#include "net_stack/http_pool.h"

namespace eego::dashboard::pipeline {

inline constexpr size_t kMaxResultMetrics = 2048;
inline constexpr size_t kMaxResultEvents = 256;
inline constexpr size_t kMaxMappingRecords = 2048;
inline constexpr size_t kMaxImageBytes = 256 * 1024;

enum class ResponseKind : uint8_t {
  GenericJson = 0,
  Prometheus,
  StandardJson,
  MetricsJson,
  EventsJson,
  Image,
  Text,
};

const char* responseKindName(ResponseKind kind);
bool responseKindFromString(std::string_view text, ResponseKind& out);

enum class WindowKind : uint8_t { Today = 0, Rolling24h, Fixed5h, Fixed7d };
const char* windowKindName(WindowKind kind);
bool windowKindFromString(std::string_view text, WindowKind& out);

enum class Aggregation : uint8_t { Sum = 0, Avg, Min, Max, Delta, Rate };
const char* aggregationName(Aggregation aggregation);
bool aggregationFromString(std::string_view text, Aggregation& out);

struct FieldMapping {
  std::string name;
  std::string path;
};

struct LabelMapping {
  std::string name;
  std::string path;
  std::string value;
};

struct MetricMapping {
  std::string name;
  std::string items_path = "$";
  std::string name_path;
  std::string value_path = "$.value";
  std::string timestamp_path = "$.timestamp_ms";
  std::string unit;
  std::string unit_path;
  std::string target_unit;
  std::string expression;
  std::string reset_at_path;
  std::vector<FieldMapping> fields;
  std::vector<LabelMapping> labels;
};

struct EventMapping {
  std::string items_path = "$.events[*]";
  std::string id_path = "$.id";
  std::string rule_id_path = "$.rule_id";
  std::string timestamp_path = "$.timestamp_ms";
  std::string severity_path = "$.severity";
  std::string state_path = "$.state";
  std::string title_path = "$.title";
  std::string message_path = "$.message";
  std::string linked_panel_id_path = "$.linked_panel_id";
  std::map<std::string, std::string> severity_map;
  std::vector<std::string> clear_states;
};

struct SourcePlan {
  Source source;
  ResponseKind response_kind = ResponseKind::StandardJson;
  std::string method = "GET";
  std::string body;
  std::string static_body;
  std::vector<MetricMapping> metrics;
  std::vector<EventMapping> events;
  std::string text_path;
  std::string reset_at_path = "$.reset_at";
  uint32_t stale_after_ms = 300000;
  bool conditional_get = true;
  size_t max_json_depth = 16;
};

struct TextData {
  std::string text;
  int64_t fetched_at_ms = 0;
};

struct ImageData {
  std::string media_type;
  std::vector<uint8_t> bytes;
  int64_t fetched_at_ms = 0;
};

struct SourceResult {
  std::string source_id;
  int64_t fetched_at_ms = 0;
  std::map<std::string, int64_t> reset_at_ms;
  std::vector<MetricSample> metrics;
  std::vector<Event> events;
  bool has_text = false;
  TextData text;
  bool has_image = false;
  ImageData image;

  int64_t resetAtFor(std::string_view metric) const;
};

struct SourceStatus {
  std::string source_id;
  int64_t last_attempt_ms = 0;
  int64_t last_success_ms = 0;
  int last_http_status = 0;
  bool offline = false;
  bool stale = true;
  uint32_t consecutive_failures = 0;
  std::string error;
  std::string cache_error;
};

struct HttpResponse {
  int status = 0;
  bool not_modified = false;
  std::string error;
  std::map<std::string, std::string> headers;
  std::string body;

  bool ok() const { return status >= 200 && status < 300; }
  const std::string* header(std::string_view lower_name) const;
};

struct AsyncHttpResult {
  uint64_t id = 0;
  HttpResponse response;
};

class IHttpTransport {
 public:
  virtual ~IHttpTransport() = default;
  virtual bool execute(const net::NetRequest& request, HttpResponse& response) = 0;
  virtual bool asyncSupported() const { return false; }
  virtual bool executeAsync(uint64_t id, const net::NetRequest& request) {
    (void)id;
    (void)request;
    return false;
  }
  virtual bool pollAsync(AsyncHttpResult& result) {
    (void)result;
    return false;
  }
  virtual bool asyncIdle() const { return true; }
  virtual void cancelAsync() {}
};

using TokenResolver = std::function<std::string(const std::string& ref)>;

}  // namespace eego::dashboard::pipeline
