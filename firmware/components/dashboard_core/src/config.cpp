// SPDX-License-Identifier: GPL-3.0-or-later
#include "dashboard_core/config.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>

#include "config_internal.h"
#include "migration.h"

namespace eego::dashboard {
namespace {

std::string fieldPath(const std::string& parent, const char* key) {
  if (parent.empty()) return key;
  return parent + "." + key;
}
std::string indexPath(const std::string& parent, size_t index) {
  return parent + "[" + std::to_string(index) + "]";
}

class Reader {
 public:
  Reader(const ParseOptions& options, std::vector<ValidationError>& errors)
      : options_(options), errors_(errors) {}

  void error(const std::string& field, const std::string& message) {
    errors_.push_back({field, message});
  }

  bool object(const JsonValue& value, const std::string& path, const char* description) {
    if (value.type() == JsonValue::Type::Object) return true;
    error(path, std::string(description) + " must be an object");
    return false;
  }

  void known(const JsonValue& object_value, const std::string& path,
             std::initializer_list<std::string_view> fields) {
    if (!options_.reject_unknown_fields || object_value.type() != JsonValue::Type::Object) return;
    for (const auto& member : object_value.objectItems()) {
      const bool found = std::find(fields.begin(), fields.end(), member.first) != fields.end();
      if (!found) error(fieldPath(path, member.first.c_str()), "unknown field");
    }
  }

  bool string(const JsonValue& object_value, const char* key, const std::string& path,
              std::string& out, size_t maximum, bool required = false) {
    const JsonValue* value = object_value.find(key);
    if (value == nullptr) {
      if (required) error(fieldPath(path, key), "missing required field");
      return !required;
    }
    if (value->type() != JsonValue::Type::String) {
      error(fieldPath(path, key), "must be a string");
      return false;
    }
    if (!jsonIsValidUtf8(value->stringValue())) {
      error(fieldPath(path, key), "must contain valid UTF-8");
      return false;
    }
    if (jsonCodePointCount(value->stringValue()) > maximum) {
      error(fieldPath(path, key), "exceeds " + std::to_string(maximum) + " code points");
      return false;
    }
    out = value->stringValue();
    return true;
  }

  bool boolean(const JsonValue& object_value, const char* key, const std::string& path,
               bool& out) {
    const JsonValue* value = object_value.find(key);
    if (value == nullptr) return true;
    if (value->type() != JsonValue::Type::Bool) {
      error(fieldPath(path, key), "must be a boolean");
      return false;
    }
    out = value->boolValue();
    return true;
  }

  bool number(const JsonValue& object_value, const char* key, const std::string& path,
              double& out, bool required = false) {
    const JsonValue* value = object_value.find(key);
    if (value == nullptr) {
      if (required) error(fieldPath(path, key), "missing required field");
      return !required;
    }
    if (value->type() != JsonValue::Type::Int && value->type() != JsonValue::Type::Double) {
      error(fieldPath(path, key), "must be a number");
      return false;
    }
    const double parsed = value->doubleValue();
    if (!std::isfinite(parsed)) {
      error(fieldPath(path, key), "must be finite");
      return false;
    }
    out = parsed;
    return true;
  }

  bool uint64(const JsonValue& object_value, const char* key, const std::string& path,
              uint64_t& out, uint64_t minimum, uint64_t maximum) {
    const JsonValue* value = object_value.find(key);
    if (value == nullptr) return true;
    if (value->type() != JsonValue::Type::Int || value->intValue() < 0) {
      error(fieldPath(path, key), "must be a non-negative integer");
      return false;
    }
    const uint64_t parsed = static_cast<uint64_t>(value->intValue());
    if (parsed < minimum || parsed > maximum) {
      error(fieldPath(path, key), "must be in " + std::to_string(minimum) + ".." +
                                      std::to_string(maximum));
      return false;
    }
    out = parsed;
    return true;
  }

  bool uint32(const JsonValue& object_value, const char* key, const std::string& path,
              uint32_t& out, uint32_t minimum, uint32_t maximum) {
    uint64_t parsed = out;
    if (!uint64(object_value, key, path, parsed, minimum, maximum)) return false;
    out = static_cast<uint32_t>(parsed);
    return true;
  }

  bool int64(const JsonValue& object_value, const char* key, const std::string& path,
             int64_t& out, int64_t minimum, int64_t maximum) {
    const JsonValue* value = object_value.find(key);
    if (value == nullptr) return true;
    if (value->type() != JsonValue::Type::Int) {
      error(fieldPath(path, key), "must be an integer");
      return false;
    }
    const int64_t parsed = value->intValue();
    if (parsed < minimum || parsed > maximum) {
      error(fieldPath(path, key), "integer out of range");
      return false;
    }
    out = parsed;
    return true;
  }

  template <typename Enum>
  bool enumeration(const JsonValue& object_value, const char* key, const std::string& path,
                   Enum& out, bool (*convert)(std::string_view, Enum&),
                   bool required = false) {
    std::string text;
    if (!string(object_value, key, path, text, 32, required)) return false;
    if (object_value.find(key) == nullptr) return true;
    if (!convert(text, out)) {
      error(fieldPath(path, key), "unsupported enum value: " + text);
      return false;
    }
    return true;
  }

  template <typename Callback>
  void array(const JsonValue& object_value, const char* key, const std::string& path,
             size_t maximum, bool required, Callback callback) {
    const JsonValue* value = object_value.find(key);
    if (value == nullptr) {
      if (required) error(fieldPath(path, key), "missing required field");
      return;
    }
    if (value->type() != JsonValue::Type::Array) {
      error(fieldPath(path, key), "must be an array");
      return;
    }
    const auto& items = value->arrayItems();
    if (items.size() > maximum)
      error(fieldPath(path, key), "exceeds maximum item count " + std::to_string(maximum));
    const size_t count = std::min(items.size(), maximum);
    for (size_t i = 0; i < count; ++i) callback(items[i], indexPath(fieldPath(path, key), i));
  }

  void stringArray(const JsonValue& object_value, const char* key, const std::string& path,
                   std::vector<std::string>& out, size_t maximum, size_t max_code_points) {
    out.clear();
    array(object_value, key, path, maximum, false,
          [&](const JsonValue& item, const std::string& item_path) {
            if (item.type() != JsonValue::Type::String) {
              error(item_path, "must be a string");
            } else if (!jsonIsValidUtf8(item.stringValue())) {
              error(item_path, "must contain valid UTF-8");
            } else if (jsonCodePointCount(item.stringValue()) > max_code_points) {
              error(item_path, "string exceeds code-point limit");
            } else {
              out.push_back(item.stringValue());
            }
          });
  }

  void keyValues(const JsonValue& object_value, const char* key, const std::string& path,
                 std::vector<KeyValue>& out, size_t maximum) {
    out.clear();
    const JsonValue* value = object_value.find(key);
    if (value == nullptr) return;
    const std::string field = fieldPath(path, key);
    if (!object(*value, field, "key/value map")) return;
    if (value->objectItems().size() > maximum)
      error(field, "exceeds maximum entry count " + std::to_string(maximum));
    const size_t count = std::min(value->objectItems().size(), maximum);
    for (size_t i = 0; i < count; ++i) {
      const auto& member = value->objectItems()[i];
      if (!jsonIsValidUtf8(member.first)) {
        error(field, "map key must contain valid UTF-8");
      } else if (member.first.empty() || jsonCodePointCount(member.first) > 64) {
        error(fieldPath(field, member.first.c_str()), "key must contain 1..64 code points");
      } else if (member.second.type() != JsonValue::Type::String) {
        error(fieldPath(field, member.first.c_str()), "value must be a string");
      } else if (!jsonIsValidUtf8(member.second.stringValue())) {
        error(fieldPath(field, member.first.c_str()), "value must contain valid UTF-8");
      } else if (jsonCodePointCount(member.second.stringValue()) > 512) {
        error(fieldPath(field, member.first.c_str()), "value exceeds 512 code points");
      } else {
        out.push_back({member.first, member.second.stringValue()});
      }
    }
  }

 private:
  const ParseOptions& options_;
  std::vector<ValidationError>& errors_;
};

void parseGrid(Reader& reader, const JsonValue& value, const std::string& path, GridRect& out) {
  out = GridRect{};
  if (!reader.object(value, path, "grid")) return;
  reader.known(value, path, {"x", "y", "w", "h"});
  reader.uint32(value, "x", path, out.x, 0, 100000);
  reader.uint32(value, "y", path, out.y, 0, 100000);
  reader.uint32(value, "w", path, out.w, 1, 256);
  reader.uint32(value, "h", path, out.h, 1, 100000);
}

void parseTimeWindow(Reader& reader, const JsonValue& value, const std::string& path,
                     TimeWindow& out) {
  out = TimeWindow{};
  if (!reader.object(value, path, "time window")) return;
  reader.known(value, path, {"from", "to"});
  reader.string(value, "from", path, out.from, 32);
  reader.string(value, "to", path, out.to, 32);
}

void parseStyle(Reader& reader, const JsonValue& value, const std::string& path, Style& out) {
  out = Style{};
  if (!reader.object(value, path, "style")) return;
  reader.known(value, path, {"unit", "variant", "precision", "line_width", "show_legend", "invert"});
  reader.string(value, "unit", path, out.unit, 32);
  reader.string(value, "variant", path, out.variant, 32);
  reader.uint32(value, "precision", path, out.precision, 0, 8);
  reader.uint32(value, "line_width", path, out.line_width, 1, 8);
  reader.boolean(value, "show_legend", path, out.show_legend);
  reader.boolean(value, "invert", path, out.invert);
}

void parseAction(Reader& reader, const JsonValue& value, const std::string& path, Action& out) {
  out = Action{};
  if (!reader.object(value, path, "action")) return;
  reader.known(value, path, {"id", "kind", "label", "target", "value", "confirm"});
  reader.string(value, "id", path, out.id, kMaxIdCodePoints, true);
  reader.enumeration(value, "kind", path, out.kind, actionKindFromString, true);
  reader.string(value, "label", path, out.label, kMaxTitleCodePoints);
  reader.string(value, "target", path, out.target, kMaxIdCodePoints);
  reader.string(value, "value", path, out.value, kMaxVariableValueCodePoints);
  reader.boolean(value, "confirm", path, out.confirm);
  if (out.kind == ActionKind::AckAlert && value.find("confirm") == nullptr) out.confirm = true;
}

void parsePanel(Reader& reader, const JsonValue& value, const std::string& path, PanelSpec& out,
                const Limits& limits) {
  out = PanelSpec{};
  if (!reader.object(value, path, "panel")) return;
  reader.known(value, path,
               {"id", "kind", "grid", "title", "visible", "source_id", "metric", "value_path",
                "value_expression", "time_window", "refresh_ms", "thresholds", "style",
                "linked_panel_ids", "actions", "text", "image", "chart", "columns"});
  reader.string(value, "id", path, out.id, kMaxIdCodePoints, true);
  reader.enumeration(value, "kind", path, out.kind, panelKindFromString, true);
  const bool text_kind = out.kind == PanelKind::Text || out.kind == PanelKind::Card ||
                         out.kind == PanelKind::Alert;
  if (value.find("text") != nullptr && !text_kind)
    reader.error(fieldPath(path, "text"), "text spec is not valid for this panel kind");
  if (value.find("image") != nullptr && out.kind != PanelKind::Image)
    reader.error(fieldPath(path, "image"), "image spec is only valid for image panels");
  if (value.find("chart") != nullptr && out.kind != PanelKind::Chart)
    reader.error(fieldPath(path, "chart"), "chart spec is only valid for chart panels");
  if (value.find("columns") != nullptr && out.kind != PanelKind::Table)
    reader.error(fieldPath(path, "columns"), "columns are only valid for table panels");
  reader.string(value, "title", path, out.title, kMaxTitleCodePoints);
  reader.boolean(value, "visible", path, out.visible);
  reader.string(value, "source_id", path, out.source_id, kMaxIdCodePoints);
  reader.string(value, "metric", path, out.metric, 128);
  reader.string(value, "value_path", path, out.value_path, 256);
  reader.string(value, "value_expression", path, out.value_expression, kMaxExpressionCodePoints);
  reader.uint32(value, "refresh_ms", path, out.refresh_ms, 0, 86400000);
  if (const JsonValue* item = value.find("grid")) parseGrid(reader, *item, fieldPath(path, "grid"), out.grid);
  if (const JsonValue* item = value.find("time_window"))
    parseTimeWindow(reader, *item, fieldPath(path, "time_window"), out.time_window);
  if (const JsonValue* item = value.find("style"))
    parseStyle(reader, *item, fieldPath(path, "style"), out.style);
  reader.stringArray(value, "linked_panel_ids", path, out.linked_panel_ids,
                     limits.max_links_per_panel, kMaxIdCodePoints);
  reader.array(value, "actions", path, limits.max_actions_per_panel, false,
               [&](const JsonValue& item, const std::string& item_path) {
                 Action action;
                 parseAction(reader, item, item_path, action);
                 out.actions.push_back(std::move(action));
               });
  reader.array(value, "thresholds", path, limits.max_thresholds_per_panel, false,
               [&](const JsonValue& item, const std::string& item_path) {
                 Threshold threshold;
                 if (reader.object(item, item_path, "threshold")) {
                   reader.known(item, item_path, {"value", "severity", "label"});
                   reader.number(item, "value", item_path, threshold.value, true);
                   reader.enumeration(item, "severity", item_path, threshold.severity,
                                      severityFromString);
                   reader.string(item, "label", item_path, threshold.label, kMaxTitleCodePoints);
                 }
                 out.thresholds.push_back(threshold);
               });
  if (const JsonValue* item = value.find("text")) {
    const std::string text_path = fieldPath(path, "text");
    if (reader.object(*item, text_path, "text spec")) {
      reader.known(*item, text_path, {"content", "multiline", "max_lines"});
      reader.string(*item, "content", text_path, out.text.content, kMaxTextCodePoints);
      reader.boolean(*item, "multiline", text_path, out.text.multiline);
      reader.uint32(*item, "max_lines", text_path, out.text.max_lines, 1, 64);
      if (out.text.multiline && item->find("max_lines") == nullptr) out.text.max_lines = 8;
    }
  }
  if (const JsonValue* item = value.find("image")) {
    const std::string image_path = fieldPath(path, "image");
    if (reader.object(*item, image_path, "image spec")) {
      reader.known(*item, image_path, {"asset_id", "alt_text", "fit"});
      reader.string(*item, "asset_id", image_path, out.image.asset_id, kMaxIdCodePoints);
      reader.string(*item, "alt_text", image_path, out.image.alt_text, kMaxTitleCodePoints);
      reader.enumeration(*item, "fit", image_path, out.image.fit, imageFitFromString);
    }
  }
  if (const JsonValue* item = value.find("chart")) {
    const std::string chart_path = fieldPath(path, "chart");
    if (reader.object(*item, chart_path, "chart spec")) {
      reader.known(*item, chart_path, {"kind", "show_points", "fill", "y_min", "y_max"});
      reader.enumeration(*item, "kind", chart_path, out.chart.kind, chartKindFromString);
      reader.boolean(*item, "show_points", chart_path, out.chart.show_points);
      reader.boolean(*item, "fill", chart_path, out.chart.fill);
      if (const JsonValue* bound = item->find("y_min"); bound != nullptr && !bound->isNull()) {
        out.chart.has_y_min = reader.number(*item, "y_min", chart_path, out.chart.y_min);
      }
      if (const JsonValue* bound = item->find("y_max"); bound != nullptr && !bound->isNull()) {
        out.chart.has_y_max = reader.number(*item, "y_max", chart_path, out.chart.y_max);
      }
    }
  }
  reader.array(value, "columns", path, limits.max_columns_per_panel, false,
               [&](const JsonValue& item, const std::string& item_path) {
                 TableColumn column;
                 if (reader.object(item, item_path, "table column")) {
                   reader.known(item, item_path, {"key", "title", "unit", "width"});
                   reader.string(item, "key", item_path, column.key, 64, true);
                   reader.string(item, "title", item_path, column.title, kMaxTitleCodePoints);
                   reader.string(item, "unit", item_path, column.unit, 32);
                   reader.uint32(item, "width", item_path, column.width, 1, 64);
                 }
                 out.columns.push_back(std::move(column));
               });
}

void parseSource(Reader& reader, const JsonValue& value, const std::string& path, Source& out) {
  out = Source{};
  if (!reader.object(value, path, "source")) return;
  reader.known(value, path,
               {"id", "kind", "enabled", "url", "refresh_ms", "timeout_ms",
                "max_response_bytes", "tls_verify", "auth_token_ref", "headers",
                "header_secret_refs"});
  reader.string(value, "id", path, out.id, kMaxIdCodePoints, true);
  reader.enumeration(value, "kind", path, out.kind, sourceKindFromString);
  reader.boolean(value, "enabled", path, out.enabled);
  reader.string(value, "url", path, out.url, kMaxUrlCodePoints);
  reader.uint32(value, "refresh_ms", path, out.refresh_ms, 1000, 86400000);
  reader.uint32(value, "timeout_ms", path, out.timeout_ms, 100, 120000);
  reader.uint32(value, "max_response_bytes", path, out.max_response_bytes, 1024, 262144);
  reader.boolean(value, "tls_verify", path, out.tls_verify);
  reader.string(value, "auth_token_ref", path, out.auth_token_ref, 128);
  reader.keyValues(value, "headers", path, out.headers, kMaxHeaders);
  reader.keyValues(value, "header_secret_refs", path, out.header_secret_refs, kMaxHeaders);
}

void parseVariable(Reader& reader, const JsonValue& value, const std::string& path, Variable& out,
                   const Limits& limits) {
  out = Variable{};
  if (!reader.object(value, path, "variable")) return;
  reader.known(value, path,
               {"name", "label", "kind", "default_value", "options", "source_id", "value_path",
                "multi", "include_all"});
  reader.string(value, "name", path, out.name, kMaxIdCodePoints, true);
  reader.string(value, "label", path, out.label, kMaxTitleCodePoints);
  reader.enumeration(value, "kind", path, out.kind, variableKindFromString);
  reader.string(value, "default_value", path, out.default_value, kMaxVariableValueCodePoints);
  reader.string(value, "source_id", path, out.source_id, kMaxIdCodePoints);
  reader.string(value, "value_path", path, out.value_path, 256);
  reader.boolean(value, "multi", path, out.multi);
  reader.boolean(value, "include_all", path, out.include_all);
  reader.array(value, "options", path, limits.max_variable_options, false,
               [&](const JsonValue& item, const std::string& item_path) {
                 VariableOption option;
                 if (reader.object(item, item_path, "variable option")) {
                   reader.known(item, item_path, {"label", "value"});
                   reader.string(item, "label", item_path, option.label, kMaxTitleCodePoints);
                   reader.string(item, "value", item_path, option.value, kMaxVariableValueCodePoints, true);
                 }
                 out.options.push_back(std::move(option));
               });
}

void parseAlertRule(Reader& reader, const JsonValue& value, const std::string& path, AlertRule& out) {
  out = AlertRule{};
  if (!reader.object(value, path, "alert rule")) return;
  reader.known(value, path,
               {"id", "title", "enabled", "source_id", "metric", "expression", "for_ms",
                "message", "style"});
  reader.string(value, "id", path, out.id, kMaxIdCodePoints, true);
  reader.string(value, "title", path, out.title, kMaxTitleCodePoints);
  reader.boolean(value, "enabled", path, out.enabled);
  reader.string(value, "source_id", path, out.source_id, kMaxIdCodePoints);
  reader.string(value, "metric", path, out.metric, 128);
  reader.string(value, "expression", path, out.expression, kMaxExpressionCodePoints, true);
  reader.uint32(value, "for_ms", path, out.for_ms, 0, 604800000);
  reader.string(value, "message", path, out.message, kMaxTextCodePoints);
  if (const JsonValue* item = value.find("style")) {
    const std::string style_path = fieldPath(path, "style");
    if (reader.object(*item, style_path, "alert style")) {
      reader.known(*item, style_path, {"surfaces", "severity", "linked_panel_id"});
      if (item->find("surfaces") != nullptr) out.style.surfaces = 0;
      reader.array(*item, "surfaces", style_path, 4, false,
                   [&](const JsonValue& surface, const std::string& surface_path) {
                     if (surface.type() != JsonValue::Type::String) {
                       reader.error(surface_path, "must be a string");
                       return;
                     }
                     uint8_t bit = 0;
                     if (!alertSurfaceFromString(surface.stringValue(), bit)) {
                       reader.error(surface_path, "unsupported alert surface");
                       return;
                     }
                     if ((out.style.surfaces & bit) != 0) reader.error(surface_path, "duplicate alert surface");
                     out.style.surfaces |= bit;
                   });
      reader.enumeration(*item, "severity", style_path, out.style.severity, severityFromString);
      reader.string(*item, "linked_panel_id", style_path, out.style.linked_panel_id, kMaxIdCodePoints);
    }
  }
}

void parseAsset(Reader& reader, const JsonValue& value, const std::string& path, Asset& out) {
  out = Asset{};
  if (!reader.object(value, path, "asset")) return;
  reader.known(value, path,
               {"id", "kind", "uri", "media_type", "sha256", "size_bytes", "cache_seconds"});
  reader.string(value, "id", path, out.id, kMaxIdCodePoints, true);
  reader.enumeration(value, "kind", path, out.kind, assetKindFromString);
  reader.string(value, "uri", path, out.uri, kMaxUrlCodePoints, true);
  reader.string(value, "media_type", path, out.media_type, 128);
  reader.string(value, "sha256", path, out.sha256, 64);
  reader.uint64(value, "size_bytes", path, out.size_bytes, 0, 64 * 1024 * 1024);
  reader.uint32(value, "cache_seconds", path, out.cache_seconds, 0, 31536000);
}

void parseSample(Reader& reader, const JsonValue& value, const std::string& path, MetricSample& out) {
  out = MetricSample{};
  if (!reader.object(value, path, "metric sample")) return;
  reader.known(value, path, {"source_id", "metric", "timestamp_ms", "value", "unit", "labels"});
  reader.string(value, "source_id", path, out.source_id, kMaxIdCodePoints);
  reader.string(value, "metric", path, out.metric, 128, true);
  reader.int64(value, "timestamp_ms", path, out.timestamp_ms, 0,
               std::numeric_limits<int64_t>::max());
  reader.number(value, "value", path, out.value, true);
  reader.string(value, "unit", path, out.unit, 32);
  reader.keyValues(value, "labels", path, out.labels, kMaxLabels);
}

void parseEvent(Reader& reader, const JsonValue& value, const std::string& path, Event& out) {
  out = Event{};
  if (!reader.object(value, path, "event")) return;
  reader.known(value, path,
               {"id", "rule_id", "source_id", "timestamp_ms", "severity", "state", "title",
                "message", "linked_panel_id"});
  reader.string(value, "id", path, out.id, kMaxIdCodePoints, true);
  reader.string(value, "rule_id", path, out.rule_id, kMaxIdCodePoints);
  reader.string(value, "source_id", path, out.source_id, kMaxIdCodePoints);
  reader.int64(value, "timestamp_ms", path, out.timestamp_ms, 0,
               std::numeric_limits<int64_t>::max());
  reader.enumeration(value, "severity", path, out.severity, severityFromString);
  reader.enumeration(value, "state", path, out.state, eventStateFromString);
  reader.string(value, "title", path, out.title, kMaxTitleCodePoints);
  reader.string(value, "message", path, out.message, kMaxTextCodePoints);
  reader.string(value, "linked_panel_id", path, out.linked_panel_id, kMaxIdCodePoints);
}

void parseV2(const JsonValue& root, Dashboard& out, const ParseOptions& options,
             std::vector<ValidationError>& errors) {
  out = Dashboard{};
  Reader reader(options, errors);
  if (!reader.object(root, "", "dashboard")) return;
  reader.known(root, "",
               {"schema", "schema_version", "id", "title", "default_page_id", "refresh_ms",
                "sources", "variables", "pages", "alert_rules", "assets", "samples", "events"});
  reader.string(root, "id", "", out.id, kMaxIdCodePoints, true);
  reader.string(root, "title", "", out.title, kMaxTitleCodePoints);
  reader.string(root, "default_page_id", "", out.default_page_id, kMaxIdCodePoints);
  reader.uint32(root, "refresh_ms", "", out.refresh_ms, 1000, 86400000);
  reader.array(root, "sources", "", options.limits.max_sources, false,
               [&](const JsonValue& item, const std::string& path) {
                 Source source;
                 parseSource(reader, item, path, source);
                 out.sources.push_back(std::move(source));
               });
  reader.array(root, "variables", "", options.limits.max_variables, false,
               [&](const JsonValue& item, const std::string& path) {
                 Variable variable;
                 parseVariable(reader, item, path, variable, options.limits);
                 out.variables.push_back(std::move(variable));
               });
  reader.array(root, "pages", "", options.limits.max_pages, true,
               [&](const JsonValue& item, const std::string& path) {
                 Page page;
                 if (reader.object(item, path, "page")) {
                   reader.known(item, path,
                                {"id", "title", "visible", "grid_columns", "row_height", "panels"});
                   reader.string(item, "id", path, page.id, kMaxIdCodePoints, true);
                   reader.string(item, "title", path, page.title, kMaxTitleCodePoints);
                   reader.boolean(item, "visible", path, page.visible);
                   reader.uint32(item, "grid_columns", path, page.grid_columns, 1, 64);
                   reader.uint32(item, "row_height", path, page.row_height, 1, 256);
                   reader.array(item, "panels", path, options.limits.max_panels_per_page, false,
                                [&](const JsonValue& panel_value, const std::string& panel_path) {
                                  PanelSpec panel;
                                  parsePanel(reader, panel_value, panel_path, panel, options.limits);
                                  page.panels.push_back(std::move(panel));
                                });
                 }
                 out.pages.push_back(std::move(page));
               });
  reader.array(root, "alert_rules", "", options.limits.max_alert_rules, false,
               [&](const JsonValue& item, const std::string& path) {
                 AlertRule rule;
                 parseAlertRule(reader, item, path, rule);
                 out.alert_rules.push_back(std::move(rule));
               });
  reader.array(root, "assets", "", options.limits.max_assets, false,
               [&](const JsonValue& item, const std::string& path) {
                 Asset asset;
                 parseAsset(reader, item, path, asset);
                 out.assets.push_back(std::move(asset));
               });
  reader.array(root, "samples", "", options.limits.max_samples, false,
               [&](const JsonValue& item, const std::string& path) {
                 MetricSample sample;
                 parseSample(reader, item, path, sample);
                 out.samples.push_back(std::move(sample));
               });
  reader.array(root, "events", "", options.limits.max_events, false,
               [&](const JsonValue& item, const std::string& path) {
                 Event event;
                 parseEvent(reader, item, path, event);
                 out.events.push_back(std::move(event));
               });
  if (out.default_page_id.empty() && !out.pages.empty()) out.default_page_id = out.pages.front().id;
}

bool checkLimitValue(size_t value, size_t maximum, const char* name, ValidationError& error) {
  if (value > 0 && value <= maximum) return true;
  error.field = std::string("limits.") + name;
  error.message = "must be in 1.." + std::to_string(maximum);
  return false;
}

bool domDepthWithin(const JsonValue& value, size_t depth, size_t maximum) {
  if (depth > maximum) return false;
  if (value.type() == JsonValue::Type::Array) {
    for (const auto& item : value.arrayItems()) {
      if (!domDepthWithin(item, depth + 1, maximum)) return false;
    }
  } else if (value.type() == JsonValue::Type::Object) {
    for (const auto& member : value.objectItems()) {
      if (!domDepthWithin(member.second, depth + 1, maximum)) return false;
    }
  }
  return true;
}

bool domKeysUnique(const JsonValue& value) {
  if (value.type() == JsonValue::Type::Array) {
    for (const auto& item : value.arrayItems()) {
      if (!domKeysUnique(item)) return false;
    }
  } else if (value.type() == JsonValue::Type::Object) {
    const auto& members = value.objectItems();
    for (size_t i = 0; i < members.size(); ++i) {
      for (size_t j = 0; j < i; ++j) {
        if (members[i].first == members[j].first) return false;
      }
      if (!domKeysUnique(members[i].second)) return false;
    }
  }
  return true;
}

bool schemaVersion(const std::string& schema, uint32_t& version) {
  if (schema == "eego.dashboard/1") {
    version = 1;
    return true;
  }
  if (schema == kSchemaName) {
    version = kSchemaVersion;
    return true;
  }
  return false;
}

}  // namespace

bool validateLimits(const Limits& limits, ValidationError& error) {
  const Limits canonical;
  return checkLimitValue(limits.max_json_bytes, canonical.max_json_bytes, "max_json_bytes", error) &&
         checkLimitValue(limits.max_depth, canonical.max_depth, "max_depth", error) &&
         checkLimitValue(limits.max_sources, canonical.max_sources, "max_sources", error) &&
         checkLimitValue(limits.max_variables, canonical.max_variables, "max_variables", error) &&
         checkLimitValue(limits.max_variable_options, canonical.max_variable_options,
                         "max_variable_options", error) &&
         checkLimitValue(limits.max_pages, canonical.max_pages, "max_pages", error) &&
         checkLimitValue(limits.max_panels_per_page, canonical.max_panels_per_page,
                         "max_panels_per_page", error) &&
         checkLimitValue(limits.max_total_panels, canonical.max_total_panels,
                         "max_total_panels", error) &&
         checkLimitValue(limits.max_actions_per_panel, canonical.max_actions_per_panel,
                         "max_actions_per_panel", error) &&
         checkLimitValue(limits.max_thresholds_per_panel, canonical.max_thresholds_per_panel,
                         "max_thresholds_per_panel", error) &&
         checkLimitValue(limits.max_links_per_panel, canonical.max_links_per_panel,
                         "max_links_per_panel", error) &&
         checkLimitValue(limits.max_columns_per_panel, canonical.max_columns_per_panel,
                         "max_columns_per_panel", error) &&
         checkLimitValue(limits.max_alert_rules, canonical.max_alert_rules, "max_alert_rules", error) &&
         checkLimitValue(limits.max_assets, canonical.max_assets, "max_assets", error) &&
         checkLimitValue(limits.max_samples, canonical.max_samples, "max_samples", error) &&
         checkLimitValue(limits.max_events, canonical.max_events, "max_events", error);
}

ParseResult parseDashboard(std::string_view json, const ParseOptions& options) {
  ParseResult result;
  ValidationError limit_error;
  if (!validateLimits(options.limits, limit_error)) {
    result.errors.push_back(std::move(limit_error));
    return result;
  }
  if (json.size() > options.limits.max_json_bytes) {
    result.errors.push_back({"", "configuration exceeds max_json_bytes"});
    return result;
  }
  JsonValue root;
  JsonParseError parse_error;
  if (!jsonParse(json, root, parse_error, options.limits.max_depth)) {
    result.errors.push_back({"", "JSON at byte " + std::to_string(parse_error.offset) + ": " +
                                     parse_error.message});
    return result;
  }
  return dashboardFromJson(root, options);
}

ParseResult dashboardFromJson(const JsonValue& root, const ParseOptions& options) {
  ParseResult result;
  ValidationError limit_error;
  if (!validateLimits(options.limits, limit_error)) {
    result.errors.push_back(std::move(limit_error));
    return result;
  }
  if (!domDepthWithin(root, 0, options.limits.max_depth)) {
    result.errors.push_back({"", "configuration exceeds max_depth"});
    return result;
  }
  if (!domKeysUnique(root)) {
    result.errors.push_back({"", "configuration contains duplicate object keys"});
    return result;
  }
  if (root.toString().size() > options.limits.max_json_bytes) {
    result.errors.push_back({"", "configuration exceeds max_json_bytes"});
    return result;
  }
  if (root.type() != JsonValue::Type::Object) {
    result.errors.push_back({"", "dashboard must be an object"});
    return result;
  }
  uint32_t version = 0;
  uint32_t schema_version = 0;
  if (const JsonValue* schema = root.find("schema")) {
    if (schema->type() != JsonValue::Type::String || !schemaVersion(schema->stringValue(), schema_version)) {
      result.errors.push_back({"schema", "unsupported dashboard schema"});
      return result;
    }
    version = schema_version;
  }
  if (const JsonValue* number = root.find("schema_version")) {
    if (number->type() != JsonValue::Type::Int ||
        (number->intValue() != 1 && number->intValue() != static_cast<int64_t>(kSchemaVersion))) {
      result.errors.push_back({"schema_version", "unsupported dashboard schema_version"});
      return result;
    }
    if (version != 0 && version != static_cast<uint32_t>(number->intValue())) {
      result.errors.push_back({"schema_version", "schema and schema_version disagree"});
      return result;
    }
    version = static_cast<uint32_t>(number->intValue());
  }
  if (version == 0) {
    result.errors.push_back({"schema_version", "missing dashboard schema version"});
    return result;
  }
  if (version == kSchemaVersion && root.find("schema") == nullptr) {
    result.errors.push_back({"schema", "missing required canonical schema"});
    return result;
  }
  if (version == kSchemaVersion && root.find("schema_version") == nullptr) {
    result.errors.push_back({"schema_version", "missing required canonical schema_version"});
    return result;
  }
  result.source_version = version;
  JsonValue migrated;
  const JsonValue* effective = &root;
  if (version == 1) {
    if (!migrateV1ToV2(root, migrated, result.errors)) return result;
    result.migrated = true;
    effective = &migrated;
  }
  parseV2(*effective, result.dashboard, options, result.errors);
  if (result.errors.empty())
    validateDashboardSemantics(result.dashboard, options.limits, result.errors);
  return result;
}

bool parseCanonicalFields(const JsonValue& root, Dashboard& out, const ParseOptions& options,
                          std::vector<ValidationError>& errors) {
  if (!domDepthWithin(root, 0, options.limits.max_depth)) {
    errors.push_back({"", "configuration exceeds max_depth"});
    return false;
  }
  if (!domKeysUnique(root)) {
    errors.push_back({"", "configuration contains duplicate object keys"});
    return false;
  }
  if (root.toString().size() > options.limits.max_json_bytes) {
    errors.push_back({"", "configuration exceeds max_json_bytes"});
    return false;
  }
  parseV2(root, out, options, errors);
  return errors.empty();
}

}  // namespace eego::dashboard
