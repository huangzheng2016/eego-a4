// SPDX-License-Identifier: GPL-3.0-or-later
#include "data_pipeline/parser.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <set>

#include "dashboard_core/expression.h"
#include "data_pipeline/json_path.h"
#include "data_pipeline/source_plan.h"

namespace eego::dashboard::pipeline {
namespace {

bool numericText(std::string_view text, double& out) {
  if (text.empty() || text.size() > 128) return false;
  const std::string copy(text);
  char* end = nullptr;
  errno = 0;
  out = std::strtod(copy.c_str(), &end);
  return errno == 0 && end == copy.c_str() + copy.size() && std::isfinite(out);
}

bool numericValue(const JsonValue& value, double& out) {
  if (value.type() == JsonValue::Type::Int || value.type() == JsonValue::Type::Double) {
    out = value.doubleValue();
    return std::isfinite(out);
  }
  return value.type() == JsonValue::Type::String && numericText(value.stringValue(), out);
}

constexpr double kLargestInt64Double = 9223372036854774784.0;

int64_t normalizeTimestamp(double value) {
  if (!std::isfinite(value) || value < 0.0) return -1;
  const double milliseconds = value < 100000000000.0 ? value * 1000.0 : value;
  if (milliseconds > kLargestInt64Double) return -1;
  return static_cast<int64_t>(milliseconds + 0.5);
}

bool leapYear(int year) {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

int daysInMonth(int year, int month) {
  static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return month == 2 && leapYear(year) ? 29 : kDays[month - 1];
}

int64_t daysFromCivil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

bool digits(std::string_view text, size_t position, size_t count, int& out) {
  if (position + count > text.size()) return false;
  out = 0;
  for (size_t i = 0; i < count; ++i) {
    const char c = text[position + i];
    if (c < '0' || c > '9') return false;
    out = out * 10 + (c - '0');
  }
  return true;
}

bool parseIso8601(std::string_view text, int64_t& timestamp_ms) {
  if (text.size() < 20 || text.size() > 40) return false;
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!digits(text, 0, 4, year) || text[4] != '-' || !digits(text, 5, 2, month) ||
      text[7] != '-' || !digits(text, 8, 2, day) ||
      (text[10] != 'T' && text[10] != 't' && text[10] != ' ') ||
      !digits(text, 11, 2, hour) || text[13] != ':' || !digits(text, 14, 2, minute) ||
      text[16] != ':' || !digits(text, 17, 2, second)) {
    return false;
  }
  if (year < 1970 || month < 1 || month > 12 || day < 1 ||
      day > daysInMonth(year, month) || hour > 23 || minute > 59 || second > 59) {
    return false;
  }
  size_t position = 19;
  int milliseconds = 0;
  if (position < text.size() && text[position] == '.') {
    ++position;
    size_t count = 0;
    while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
      if (count < 3) milliseconds = milliseconds * 10 + (text[position] - '0');
      ++count;
      ++position;
    }
    if (count == 0 || count > 9) return false;
    while (count++ < 3) milliseconds *= 10;
  }
  int offset_minutes = 0;
  if (position < text.size() && (text[position] == 'Z' || text[position] == 'z')) {
    ++position;
  } else if (position < text.size() && (text[position] == '+' || text[position] == '-')) {
    const int sign = text[position++] == '+' ? 1 : -1;
    int offset_hour = 0;
    int offset_minute = 0;
    if (!digits(text, position, 2, offset_hour)) return false;
    position += 2;
    if (position < text.size() && text[position] == ':') ++position;
    if (!digits(text, position, 2, offset_minute)) return false;
    position += 2;
    if (offset_hour > 23 || offset_minute > 59) return false;
    offset_minutes = sign * (offset_hour * 60 + offset_minute);
  } else {
    return false;
  }
  if (position != text.size()) return false;
  const int64_t days = daysFromCivil(year, static_cast<unsigned>(month),
                                     static_cast<unsigned>(day));
  const int64_t seconds = days * 86400 + hour * 3600 + minute * 60 + second -
                          static_cast<int64_t>(offset_minutes) * 60;
  if (seconds < 0 || seconds > std::numeric_limits<int64_t>::max() / 1000) return false;
  timestamp_ms = seconds * 1000 + milliseconds;
  return true;
}

const JsonValue* first(const JsonValue& root, const std::string& path) {
  return path.empty() ? nullptr : jsonPathFirst(root, path);
}

bool parseMappedTimestamp(const JsonValue& value, bool explicit_milliseconds, int64_t& out) {
  if (explicit_milliseconds) {
    double number = 0.0;
    if (numericValue(value, number) && number >= 0.0 && number <= kLargestInt64Double) {
      out = static_cast<int64_t>(number + 0.5);
      return true;
    }
  }
  return parseTimestamp(value, out);
}

bool selectRecords(const JsonValue& root, const std::string& path,
                   std::vector<const JsonValue*>& out, std::string& error) {
  std::vector<const JsonValue*> selected;
  if (!selectJsonPath(root, path, selected, error)) return false;
  out.clear();
  for (const JsonValue* value : selected) {
    if (value->type() == JsonValue::Type::Array) {
      for (const JsonValue& item : value->arrayItems()) out.push_back(&item);
    } else {
      out.push_back(value);
    }
    if (out.size() > kMaxMappingRecords) {
      error = "mapped record count exceeds configured limit";
      return false;
    }
  }
  return true;
}

EvalValue evalValue(const JsonValue* value) {
  if (value == nullptr || value->isNull()) return EvalValue::makeNull();
  if (value->type() == JsonValue::Type::Bool) return EvalValue::makeBool(value->boolValue());
  if (value->type() == JsonValue::Type::Int || value->type() == JsonValue::Type::Double)
    return EvalValue::makeNumber(value->doubleValue());
  if (value->type() == JsonValue::Type::String)
    return EvalValue::makeString(value->stringValue());
  return EvalValue::makeString(value->toString());
}

void addVariableValues(EvalValues& values, const VariableValues& variables) {
  for (const auto& variable : variables) {
    double number = 0.0;
    if (numericText(variable.second, number))
      values.emplace_back(variable.first, EvalValue::makeNumber(number));
    else
      values.emplace_back(variable.first, EvalValue::makeString(variable.second));
  }
}

bool transformValue(double& value, const MetricMapping& mapping, const JsonValue& record,
                    int64_t timestamp_ms, int64_t reset_at_ms,
                    const VariableValues& variables, std::string& error) {
  if (mapping.expression.empty()) return true;
  EvalValues values;
  values.emplace_back("value", EvalValue::makeNumber(value));
  values.emplace_back("timestamp", EvalValue::makeNumber(static_cast<double>(timestamp_ms)));
  values.emplace_back("reset_at", EvalValue::makeNumber(static_cast<double>(reset_at_ms)));
  for (const FieldMapping& field : mapping.fields)
    values.emplace_back(field.name, evalValue(first(record, field.path)));
  addVariableValues(values, variables);
  ExpressionError expression_error;
  if (!evaluateNumericExpression(mapping.expression, values, value, expression_error)) {
    error = "metric expression failed at byte " + std::to_string(expression_error.offset) +
            ": " + expression_error.message;
    return false;
  }
  return true;
}

bool convertSampleUnit(MetricSample& sample, const MetricMapping& mapping,
                       const std::string& source_unit, std::string& error) {
  if (mapping.target_unit.empty()) return true;
  if (source_unit.empty() ||
      !convertUnit(sample.value, source_unit, mapping.target_unit, sample.value)) {
    error = "unknown or incompatible unit conversion for metric " + sample.metric;
    return false;
  }
  sample.unit = mapping.target_unit;
  return true;
}

bool mapMetric(const SourcePlan& plan, const MetricMapping& mapping, const JsonValue& record,
               const VariableValues& variables, int64_t now_ms, bool standard_labels,
               SourceResult& out, std::string& error) {
  const JsonValue* name_value = first(record, mapping.name_path);
  const std::string name = mapping.name_path.empty() ? mapping.name
                           : name_value != nullptr && name_value->isScalar()
                               ? name_value->scalarText()
                               : std::string();
  if (name.empty() || !jsonIsValidUtf8(name) || jsonCodePointCount(name) > 128) {
    error = "mapped metric name is missing, invalid, or too long";
    return false;
  }
  const JsonValue* value_node = first(record, mapping.value_path);
  double value = 0.0;
  if (value_node == nullptr || !numericValue(*value_node, value)) {
    error = "mapped metric value is not a finite number: " + name;
    return false;
  }

  int64_t timestamp_ms = now_ms;
  bool explicit_milliseconds = mapping.timestamp_path == "$.timestamp_ms";
  const JsonValue* timestamp = first(record, mapping.timestamp_path);
  if (timestamp == nullptr && mapping.timestamp_path == "$.timestamp_ms") {
    explicit_milliseconds = false;
    for (const char* fallback : {"$.timestamp", "$.ts", "$.time"}) {
      timestamp = first(record, fallback);
      if (timestamp != nullptr) break;
    }
  }
  if (timestamp != nullptr &&
      !parseMappedTimestamp(*timestamp, explicit_milliseconds, timestamp_ms)) {
    error = "invalid metric timestamp: " + name;
    return false;
  }

  int64_t reset_at_ms = 0;
  if (const JsonValue* reset = first(record, mapping.reset_at_path)) {
    if (!parseTimestamp(*reset, reset_at_ms)) {
      error = "invalid metric reset_at: " + name;
      return false;
    }
  }

  MetricSample sample;
  sample.source_id = plan.source.id;
  sample.metric = name;
  sample.timestamp_ms = timestamp_ms;
  sample.value = value;
  const JsonValue* unit_node = first(record, mapping.unit_path);
  const std::string source_unit = unit_node != nullptr && unit_node->isScalar()
                                      ? unit_node->scalarText()
                                      : mapping.unit;
  sample.unit = mapping.target_unit.empty() ? source_unit : mapping.target_unit;
  if (!jsonIsValidUtf8(sample.unit) || jsonCodePointCount(sample.unit) > 32) {
    error = "metric unit exceeds its budget: " + name;
    return false;
  }

  std::set<std::string> label_names;
  for (const LabelMapping& label : mapping.labels) {
    std::string value_text;
    if (!label.path.empty()) {
      const JsonValue* label_value = first(record, label.path);
      if (label_value == nullptr || !label_value->isScalar()) continue;
      value_text = label_value->scalarText();
    } else if (!interpolateVariables(label.value, variables, value_text, error, 512)) {
      return false;
    }
    if (!jsonIsValidUtf8(value_text) || jsonCodePointCount(value_text) > 512) {
      error = "mapped label value is invalid or too long";
      return false;
    }
    label_names.insert(label.name);
    sample.labels.push_back({label.name, std::move(value_text)});
  }
  if (standard_labels) {
    if (const JsonValue* labels = first(record, "$.labels")) {
      if (labels->type() != JsonValue::Type::Object) {
        error = "standard metric labels must be an object";
        return false;
      }
      for (const auto& member : labels->objectItems()) {
        if (!member.second.isScalar() || member.first.empty() || member.first.size() > 64 ||
            label_names.count(member.first) != 0) {
          error = "invalid or duplicate standard metric label";
          return false;
        }
        const std::string label_value = member.second.scalarText();
        if (jsonCodePointCount(label_value) > 512) {
          error = "standard metric label value is too long";
          return false;
        }
        sample.labels.push_back({member.first, label_value});
      }
    }
  }
  if (sample.labels.size() > kMaxLabels) {
    error = "metric exceeds label count limit: " + name;
    return false;
  }

  if (!transformValue(sample.value, mapping, record, timestamp_ms, reset_at_ms, variables,
                      error) ||
      !convertSampleUnit(sample, mapping, source_unit, error)) {
    return false;
  }
  if (out.metrics.size() >= kMaxResultMetrics) {
    error = "normalized metric count exceeds configured limit";
    return false;
  }
  if (reset_at_ms > 0) out.reset_at_ms[name] = reset_at_ms;
  out.metrics.push_back(std::move(sample));
  return true;
}

uint64_t fnv1a(std::string_view text, uint64_t hash = 1469598103934665603ULL) {
  for (const unsigned char c : text) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::string eventId(const std::string& source_id, const std::string& raw,
                    const JsonValue& record) {
  const bool valid = !raw.empty() && raw.size() <= 64 &&
                     std::all_of(raw.begin(), raw.end(), [](char c) {
                       return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                              (c >= '0' && c <= '9') || c == '_' || c == '-';
                     });
  if (valid) return raw;
  uint64_t hash = fnv1a(source_id);
  hash = fnv1a(raw, hash);
  hash = fnv1a(record.toString(), hash);
  char buffer[17];
  static constexpr char kHex[] = "0123456789abcdef";
  for (size_t i = 0; i < 16; ++i) buffer[i] = kHex[(hash >> ((15 - i) * 4)) & 0xf];
  buffer[16] = '\0';
  return "ev-" + std::string(buffer);
}

std::string scalarAt(const JsonValue& record, const std::string& path) {
  const JsonValue* value = first(record, path);
  return value != nullptr && value->isScalar() ? value->scalarText() : std::string();
}

bool validReference(std::string_view id) {
  return id.size() <= 64 &&
         std::all_of(id.begin(), id.end(), [](char c) {
           return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-';
         });
}

bool mapEvent(const SourcePlan& plan, const EventMapping& mapping, const JsonValue& record,
              int64_t now_ms, SourceResult& out, std::string& error) {
  if (record.type() != JsonValue::Type::Object) {
    error = "mapped event record must be an object";
    return false;
  }
  Event event;
  event.source_id = plan.source.id;
  event.id = eventId(plan.source.id, scalarAt(record, mapping.id_path), record);
  event.rule_id = scalarAt(record, mapping.rule_id_path);
  event.linked_panel_id = scalarAt(record, mapping.linked_panel_id_path);
  if (!validReference(event.rule_id) || !validReference(event.linked_panel_id)) {
    error = "event rule/panel reference is invalid";
    return false;
  }
  event.timestamp_ms = now_ms;
  bool explicit_milliseconds = mapping.timestamp_path == "$.timestamp_ms";
  const JsonValue* timestamp = first(record, mapping.timestamp_path);
  if (timestamp == nullptr && mapping.timestamp_path == "$.timestamp_ms") {
    explicit_milliseconds = false;
    for (const char* fallback : {"$.timestamp", "$.ts", "$.time"}) {
      timestamp = first(record, fallback);
      if (timestamp != nullptr) break;
    }
  }
  if (timestamp != nullptr &&
      !parseMappedTimestamp(*timestamp, explicit_milliseconds, event.timestamp_ms)) {
    error = "invalid event timestamp";
    return false;
  }
  std::string severity = scalarAt(record, mapping.severity_path);
  const auto mapped = mapping.severity_map.find(severity);
  if (mapped != mapping.severity_map.end()) severity = mapped->second;
  if (!severity.empty() && !severityFromString(severity, event.severity)) {
    error = "event severity is not canonical after mapping";
    return false;
  }
  const std::string state = scalarAt(record, mapping.state_path);
  EventState parsed_state = EventState::Active;
  if (eventStateFromString(state, parsed_state)) {
    event.state = parsed_state;
  } else {
    static const std::vector<std::string> kDefaultClears = {"cleared", "resolved", "inactive"};
    const auto& clears = mapping.clear_states.empty() ? kDefaultClears : mapping.clear_states;
    if (std::find(clears.begin(), clears.end(), state) != clears.end())
      event.state = EventState::Cleared;
  }
  event.title = scalarAt(record, mapping.title_path);
  event.message = scalarAt(record, mapping.message_path);
  if (!jsonIsValidUtf8(event.title) || !jsonIsValidUtf8(event.message) ||
      jsonCodePointCount(event.title) > kMaxTitleCodePoints ||
      jsonCodePointCount(event.message) > kMaxTextCodePoints) {
    error = "event title/message is invalid UTF-8 or exceeds its budget";
    return false;
  }
  if (out.events.size() >= kMaxResultEvents) {
    error = "normalized event count exceeds configured limit";
    return false;
  }
  out.events.push_back(std::move(event));
  return true;
}

bool setText(const std::string& text, int64_t now_ms, SourceResult& out, std::string& error) {
  if (!jsonIsValidUtf8(text) || jsonCodePointCount(text) > kMaxTextCodePoints) {
    error = "text response is invalid UTF-8 or exceeds the text budget";
    return false;
  }
  out.has_text = true;
  out.text.text = text;
  out.text.fetched_at_ms = now_ms;
  return true;
}

bool parseStandard(const SourcePlan& plan, const JsonValue& root,
                   const VariableValues& variables, int64_t now_ms, bool metrics,
                   bool events, SourceResult& out, std::string& error) {
  if (plan.response_kind == ResponseKind::StandardJson &&
      root.type() != JsonValue::Type::Object) {
    error = "standard_json response must be an object";
    return false;
  }
  if (metrics) {
    const JsonValue* metric_root = &root;
    if (root.type() == JsonValue::Type::Object) {
      metric_root = root.find("metrics");
      if (metric_root == nullptr && plan.response_kind == ResponseKind::MetricsJson) {
        error = "metrics_json response has no metrics array";
        return false;
      }
    }
    if (metric_root != nullptr) {
      MetricMapping mapping;
      mapping.items_path = "$";
      mapping.name_path = "$.metric";
      mapping.unit_path = "$.unit";
      mapping.reset_at_path = "$.reset_at";
      std::vector<const JsonValue*> records;
      if (!selectRecords(*metric_root, "$", records, error)) return false;
      for (const JsonValue* record : records) {
        if (record->type() != JsonValue::Type::Object) {
          error = "standard metric record must be an object";
          return false;
        }
        if (!mapMetric(plan, mapping, *record, variables, now_ms, true, out, error)) return false;
      }
    }
  }
  if (events) {
    const JsonValue* event_root = &root;
    if (root.type() == JsonValue::Type::Object) {
      event_root = root.find("events");
      if (event_root == nullptr && plan.response_kind == ResponseKind::EventsJson) {
        error = "events_json response has no events array";
        return false;
      }
    }
    if (event_root != nullptr) {
      EventMapping mapping;
      mapping.items_path = "$";
      std::vector<const JsonValue*> records;
      if (!selectRecords(*event_root, "$", records, error)) return false;
      for (const JsonValue* record : records) {
        if (!mapEvent(plan, mapping, *record, now_ms, out, error)) return false;
      }
    }
  }
  if (plan.response_kind == ResponseKind::StandardJson) {
    if (const JsonValue* text = root.find("text")) {
      if (text->type() != JsonValue::Type::String) {
        error = "standard text value must be a string";
        return false;
      }
      if (!setText(text->stringValue(), now_ms, out, error)) return false;
    }
  }
  return true;
}

bool promPair(const JsonValue& value, int64_t& timestamp_ms, const JsonValue*& payload,
              std::string& error) {
  if (value.type() != JsonValue::Type::Array || value.arrayItems().size() != 2 ||
      !parseTimestamp(value.arrayItems()[0], timestamp_ms)) {
    error = "invalid Prometheus [timestamp, value] pair";
    return false;
  }
  payload = &value.arrayItems()[1];
  return true;
}

bool promLabels(const JsonValue& item, const MetricMapping& mapping, MetricSample& sample,
                std::string& error) {
  sample.metric = mapping.name;
  if (const JsonValue* metric = item.find("metric")) {
    if (metric->type() != JsonValue::Type::Object) {
      error = "Prometheus metric labels must be an object";
      return false;
    }
    for (const auto& member : metric->objectItems()) {
      if (member.first == "__name__") {
        if (!member.second.isScalar()) {
          error = "Prometheus __name__ must be scalar";
          return false;
        }
        sample.metric = member.second.scalarText();
      } else {
        if (!member.second.isScalar() || member.first.empty() || member.first.size() > 64) {
          error = "invalid Prometheus label";
          return false;
        }
        sample.labels.push_back({member.first, member.second.scalarText()});
      }
    }
  }
  if (sample.metric.empty()) sample.metric = "prometheus.value";
  if (!jsonIsValidUtf8(sample.metric) || jsonCodePointCount(sample.metric) > 128 ||
      sample.labels.size() > kMaxLabels) {
    error = "Prometheus metric exceeds name/label budget";
    return false;
  }
  return true;
}

bool appendPromSample(const SourcePlan& plan, const MetricMapping& mapping,
                      const JsonValue& record, const JsonValue& pair, MetricSample sample,
                      const VariableValues& variables, SourceResult& out, std::string& error) {
  const JsonValue* payload = nullptr;
  if (!promPair(pair, sample.timestamp_ms, payload, error) ||
      !numericValue(*payload, sample.value)) {
    if (error.empty()) error = "Prometheus sample value is not numeric";
    return false;
  }
  sample.source_id = plan.source.id;
  sample.unit = mapping.target_unit.empty() ? mapping.unit : mapping.target_unit;
  if (!jsonIsValidUtf8(sample.unit) || jsonCodePointCount(sample.unit) > 32) {
    error = "Prometheus metric exceeds unit budget";
    return false;
  }
  const int64_t reset_at_ms = out.resetAtFor(sample.metric);
  if (!transformValue(sample.value, mapping, record, sample.timestamp_ms, reset_at_ms, variables,
                      error) ||
      !convertSampleUnit(sample, mapping, mapping.unit, error)) {
    return false;
  }
  if (out.metrics.size() >= kMaxResultMetrics) {
    error = "Prometheus result exceeds metric point limit";
    return false;
  }
  out.metrics.push_back(std::move(sample));
  return true;
}

bool parsePrometheus(const SourcePlan& plan, const JsonValue& root,
                     const VariableValues& variables, SourceResult& out, std::string& error) {
  const JsonValue* status = root.find("status");
  const JsonValue* data = root.find("data");
  if (status == nullptr || status->type() != JsonValue::Type::String ||
      status->stringValue() != "success" || data == nullptr ||
      data->type() != JsonValue::Type::Object) {
    error = "Prometheus response is not a successful data envelope";
    return false;
  }
  const JsonValue* type = data->find("resultType");
  const JsonValue* result = data->find("result");
  if (type == nullptr || type->type() != JsonValue::Type::String || result == nullptr) {
    error = "Prometheus response lacks resultType/result";
    return false;
  }
  MetricMapping mapping;
  if (!plan.metrics.empty()) mapping = plan.metrics.front();
  const std::string& result_type = type->stringValue();
  if (result_type == "scalar" || result_type == "string") {
    int64_t timestamp_ms = 0;
    const JsonValue* payload = nullptr;
    if (!promPair(*result, timestamp_ms, payload, error)) return false;
    if (result_type == "string") {
      if (payload->type() != JsonValue::Type::String) {
        error = "Prometheus string result is not a string";
        return false;
      }
      return setText(payload->stringValue(), timestamp_ms, out, error);
    }
    MetricSample sample;
    sample.metric = mapping.name.empty() ? "prometheus.value" : mapping.name;
    return appendPromSample(plan, mapping, *data, *result, std::move(sample), variables, out,
                            error);
  }
  if (result_type != "vector" && result_type != "matrix") {
    error = "unsupported Prometheus resultType: " + result_type;
    return false;
  }
  if (result->type() != JsonValue::Type::Array) {
    error = "Prometheus vector/matrix result must be an array";
    return false;
  }
  for (const JsonValue& item : result->arrayItems()) {
    if (item.type() != JsonValue::Type::Object) {
      error = "Prometheus series must be an object";
      return false;
    }
    MetricSample sample;
    if (!promLabels(item, mapping, sample, error)) return false;
    if (result_type == "vector") {
      const JsonValue* value = item.find("value");
      if (value == nullptr ||
          !appendPromSample(plan, mapping, item, *value, std::move(sample), variables, out,
                            error)) {
        if (error.empty()) error = "Prometheus vector series lacks value";
        return false;
      }
    } else {
      const JsonValue* values = item.find("values");
      if (values == nullptr || values->type() != JsonValue::Type::Array) {
        error = "Prometheus matrix series lacks values";
        return false;
      }
      for (const JsonValue& value : values->arrayItems()) {
        MetricSample point = sample;
        if (!appendPromSample(plan, mapping, item, value, std::move(point), variables, out, error))
          return false;
      }
    }
  }
  return true;
}

}  // namespace

bool parseTimestamp(const JsonValue& value, int64_t& timestamp_ms) {
  double number = 0.0;
  if (numericValue(value, number)) {
    timestamp_ms = normalizeTimestamp(number);
    return timestamp_ms >= 0;
  }
  return value.type() == JsonValue::Type::String &&
         parseIso8601(value.stringValue(), timestamp_ms);
}

bool parseSourceResponse(const SourcePlan& plan, const HttpResponse& response,
                         const VariableValues& variables, int64_t now_ms,
                         SourceResult& out, std::string& error) {
  out = SourceResult{};
  out.source_id = plan.source.id;
  out.fetched_at_ms = now_ms;
  error.clear();
  if (!validateSourcePlan(plan, error)) return false;
  if (!response.ok()) {
    error = response.error.empty() ? "HTTP status " + std::to_string(response.status)
                                   : response.error;
    return false;
  }
  if (response.body.size() > plan.source.max_response_bytes) {
    error = "response exceeds source byte budget";
    return false;
  }
  if (plan.response_kind == ResponseKind::Image) {
    if (response.body.empty() || response.body.size() > kMaxImageBytes) {
      error = "image response is empty or exceeds the image budget";
      return false;
    }
    std::string media_type;
    if (const std::string* content_type = response.header("content-type")) {
      media_type = content_type->substr(0, content_type->find(';'));
      while (!media_type.empty() && media_type.back() == ' ') media_type.pop_back();
    }
    if (media_type.empty()) {
      const auto* bytes = reinterpret_cast<const unsigned char*>(response.body.data());
      if (response.body.size() >= 8 && std::memcmp(bytes, "\x89PNG\r\n\x1a\n", 8) == 0)
        media_type = "image/png";
      else if (response.body.size() >= 2 && bytes[0] == 0xff && bytes[1] == 0xd8)
        media_type = "image/jpeg";
    }
    if (media_type.rfind("image/", 0) != 0) {
      error = "image response has no supported image media type";
      return false;
    }
    out.has_image = true;
    out.image.media_type = std::move(media_type);
    out.image.bytes.assign(response.body.begin(), response.body.end());
    out.image.fetched_at_ms = now_ms;
    return true;
  }
  if (plan.response_kind == ResponseKind::Text)
    return setText(response.body, now_ms, out, error);

  JsonValue root;
  JsonParseError parse_error;
  if (!jsonParse(response.body, root, parse_error, plan.max_json_depth)) {
    error = "JSON parse error at byte " + std::to_string(parse_error.offset) + ": " +
            parse_error.message;
    return false;
  }
  if (const JsonValue* reset = first(root, plan.reset_at_path)) {
    int64_t reset_at_ms = 0;
    if (!parseTimestamp(*reset, reset_at_ms)) {
      error = "invalid source reset_at";
      return false;
    }
    out.reset_at_ms["*"] = reset_at_ms;
  }

  switch (plan.response_kind) {
    case ResponseKind::GenericJson: {
      for (const MetricMapping& mapping : plan.metrics) {
        std::vector<const JsonValue*> records;
        if (!selectRecords(root, mapping.items_path, records, error)) return false;
        for (const JsonValue* record : records) {
          if (!mapMetric(plan, mapping, *record, variables, now_ms, false, out, error))
            return false;
        }
      }
      for (const EventMapping& mapping : plan.events) {
        std::vector<const JsonValue*> records;
        if (!selectRecords(root, mapping.items_path, records, error)) return false;
        for (const JsonValue* record : records) {
          if (!mapEvent(plan, mapping, *record, now_ms, out, error)) return false;
        }
      }
      if (!plan.text_path.empty()) {
        const JsonValue* text = first(root, plan.text_path);
        if (text == nullptr || text->type() != JsonValue::Type::String) {
          error = "mapped text value is missing or not a string";
          return false;
        }
        if (!setText(text->stringValue(), now_ms, out, error)) return false;
      }
      return true;
    }
    case ResponseKind::Prometheus:
      return parsePrometheus(plan, root, variables, out, error);
    case ResponseKind::StandardJson:
      return parseStandard(plan, root, variables, now_ms, true, true, out, error);
    case ResponseKind::MetricsJson:
      return parseStandard(plan, root, variables, now_ms, true, false, out, error);
    case ResponseKind::EventsJson:
      return parseStandard(plan, root, variables, now_ms, false, true, out, error);
    case ResponseKind::Image:
    case ResponseKind::Text:
      break;
  }
  error = "unsupported response kind";
  return false;
}

}  // namespace eego::dashboard::pipeline
