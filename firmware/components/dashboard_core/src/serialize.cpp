// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <utility>

#include "dashboard_core/config.h"

namespace eego::dashboard {
namespace {

JsonValue string(const std::string& value) { return JsonValue::makeString(value); }
JsonValue boolean(bool value) { return JsonValue::makeBool(value); }
JsonValue integer(int64_t value) { return JsonValue::makeInt(value); }
JsonValue number(double value) { return JsonValue::makeDouble(value); }

JsonValue keyValuesToJson(const std::vector<KeyValue>& values) {
  JsonValue::Object object;
  for (const auto& value : values) object.emplace_back(value.key, string(value.value));
  return JsonValue::makeObject(std::move(object));
}

JsonValue gridToJson(const GridRect& grid) {
  JsonValue::Object object;
  object.emplace_back("x", integer(grid.x));
  object.emplace_back("y", integer(grid.y));
  object.emplace_back("w", integer(grid.w));
  object.emplace_back("h", integer(grid.h));
  return JsonValue::makeObject(std::move(object));
}

JsonValue timeWindowToJson(const TimeWindow& window) {
  JsonValue::Object object;
  object.emplace_back("from", string(window.from));
  object.emplace_back("to", string(window.to));
  return JsonValue::makeObject(std::move(object));
}

JsonValue styleToJson(const Style& style) {
  JsonValue::Object object;
  object.emplace_back("unit", string(style.unit));
  object.emplace_back("variant", string(style.variant));
  object.emplace_back("precision", integer(style.precision));
  object.emplace_back("line_width", integer(style.line_width));
  object.emplace_back("show_legend", boolean(style.show_legend));
  object.emplace_back("invert", boolean(style.invert));
  return JsonValue::makeObject(std::move(object));
}

JsonValue actionToJson(const Action& action) {
  JsonValue::Object object;
  object.emplace_back("id", string(action.id));
  object.emplace_back("kind", string(actionKindName(action.kind)));
  object.emplace_back("label", string(action.label));
  object.emplace_back("target", string(action.target));
  object.emplace_back("value", string(action.value));
  object.emplace_back("confirm", boolean(action.confirm));
  return JsonValue::makeObject(std::move(object));
}

JsonValue panelToJson(const PanelSpec& panel) {
  JsonValue::Object object;
  object.emplace_back("id", string(panel.id));
  object.emplace_back("kind", string(panelKindName(panel.kind)));
  object.emplace_back("grid", gridToJson(panel.grid));
  object.emplace_back("title", string(panel.title));
  object.emplace_back("visible", boolean(panel.visible));
  object.emplace_back("source_id", string(panel.source_id));
  object.emplace_back("metric", string(panel.metric));
  object.emplace_back("value_path", string(panel.value_path));
  object.emplace_back("value_expression", string(panel.value_expression));
  object.emplace_back("time_window", timeWindowToJson(panel.time_window));
  object.emplace_back("refresh_ms", integer(panel.refresh_ms));
  JsonValue::Array thresholds;
  for (const auto& threshold : panel.thresholds) {
    JsonValue::Object item;
    item.emplace_back("value", number(threshold.value));
    item.emplace_back("severity", string(severityName(threshold.severity)));
    item.emplace_back("label", string(threshold.label));
    thresholds.push_back(JsonValue::makeObject(std::move(item)));
  }
  object.emplace_back("thresholds", JsonValue::makeArray(std::move(thresholds)));
  object.emplace_back("style", styleToJson(panel.style));
  JsonValue::Array links;
  for (const auto& link : panel.linked_panel_ids) links.push_back(string(link));
  object.emplace_back("linked_panel_ids", JsonValue::makeArray(std::move(links)));
  JsonValue::Array actions;
  for (const auto& action : panel.actions) actions.push_back(actionToJson(action));
  object.emplace_back("actions", JsonValue::makeArray(std::move(actions)));
  if (panel.kind == PanelKind::Text || panel.kind == PanelKind::Card ||
      panel.kind == PanelKind::Alert) {
    JsonValue::Object text;
    text.emplace_back("content", string(panel.text.content));
    text.emplace_back("multiline", boolean(panel.text.multiline));
    text.emplace_back("max_lines", integer(panel.text.max_lines));
    object.emplace_back("text", JsonValue::makeObject(std::move(text)));
  }
  if (panel.kind == PanelKind::Image) {
    JsonValue::Object image;
    image.emplace_back("asset_id", string(panel.image.asset_id));
    image.emplace_back("alt_text", string(panel.image.alt_text));
    image.emplace_back("fit", string(imageFitName(panel.image.fit)));
    object.emplace_back("image", JsonValue::makeObject(std::move(image)));
  }
  if (panel.kind == PanelKind::Chart) {
    JsonValue::Object chart;
    chart.emplace_back("kind", string(chartKindName(panel.chart.kind)));
    chart.emplace_back("show_points", boolean(panel.chart.show_points));
    chart.emplace_back("fill", boolean(panel.chart.fill));
    chart.emplace_back("y_min", panel.chart.has_y_min ? number(panel.chart.y_min) : JsonValue());
    chart.emplace_back("y_max", panel.chart.has_y_max ? number(panel.chart.y_max) : JsonValue());
    object.emplace_back("chart", JsonValue::makeObject(std::move(chart)));
  }
  if (panel.kind == PanelKind::Table) {
    JsonValue::Array columns;
    for (const auto& column : panel.columns) {
      JsonValue::Object item;
      item.emplace_back("key", string(column.key));
      item.emplace_back("title", string(column.title));
      item.emplace_back("unit", string(column.unit));
      item.emplace_back("width", integer(column.width));
      columns.push_back(JsonValue::makeObject(std::move(item)));
    }
    object.emplace_back("columns", JsonValue::makeArray(std::move(columns)));
  }
  return JsonValue::makeObject(std::move(object));
}

JsonValue sourceToJson(const Source& source) {
  JsonValue::Object object;
  object.emplace_back("id", string(source.id));
  object.emplace_back("kind", string(sourceKindName(source.kind)));
  object.emplace_back("enabled", boolean(source.enabled));
  object.emplace_back("url", string(source.url));
  object.emplace_back("refresh_ms", integer(source.refresh_ms));
  object.emplace_back("timeout_ms", integer(source.timeout_ms));
  object.emplace_back("max_response_bytes", integer(source.max_response_bytes));
  object.emplace_back("tls_verify", boolean(source.tls_verify));
  object.emplace_back("auth_token_ref", string(source.auth_token_ref));
  object.emplace_back("headers", keyValuesToJson(source.headers));
  return JsonValue::makeObject(std::move(object));
}

JsonValue variableToJson(const Variable& variable) {
  JsonValue::Object object;
  object.emplace_back("name", string(variable.name));
  object.emplace_back("label", string(variable.label));
  object.emplace_back("kind", string(variableKindName(variable.kind)));
  object.emplace_back("default_value", string(variable.default_value));
  JsonValue::Array options;
  for (const auto& option : variable.options) {
    JsonValue::Object item;
    item.emplace_back("label", string(option.label));
    item.emplace_back("value", string(option.value));
    options.push_back(JsonValue::makeObject(std::move(item)));
  }
  object.emplace_back("options", JsonValue::makeArray(std::move(options)));
  object.emplace_back("source_id", string(variable.source_id));
  object.emplace_back("value_path", string(variable.value_path));
  object.emplace_back("multi", boolean(variable.multi));
  object.emplace_back("include_all", boolean(variable.include_all));
  return JsonValue::makeObject(std::move(object));
}

JsonValue alertRuleToJson(const AlertRule& rule) {
  JsonValue::Object object;
  object.emplace_back("id", string(rule.id));
  object.emplace_back("title", string(rule.title));
  object.emplace_back("enabled", boolean(rule.enabled));
  object.emplace_back("source_id", string(rule.source_id));
  object.emplace_back("metric", string(rule.metric));
  object.emplace_back("expression", string(rule.expression));
  object.emplace_back("for_ms", integer(rule.for_ms));
  object.emplace_back("message", string(rule.message));
  JsonValue::Object style;
  JsonValue::Array surfaces;
  for (const uint8_t bit : {kAlertSurfaceBanner, kAlertSurfaceCard, kAlertSurfaceTableRow,
                            kAlertSurfaceSparkline}) {
    if ((rule.style.surfaces & bit) != 0) surfaces.push_back(string(alertSurfaceName(bit)));
  }
  style.emplace_back("surfaces", JsonValue::makeArray(std::move(surfaces)));
  style.emplace_back("severity", string(severityName(rule.style.severity)));
  style.emplace_back("linked_panel_id", string(rule.style.linked_panel_id));
  object.emplace_back("style", JsonValue::makeObject(std::move(style)));
  return JsonValue::makeObject(std::move(object));
}

JsonValue assetToJson(const Asset& asset) {
  JsonValue::Object object;
  object.emplace_back("id", string(asset.id));
  object.emplace_back("kind", string(assetKindName(asset.kind)));
  object.emplace_back("uri", string(asset.uri));
  object.emplace_back("media_type", string(asset.media_type));
  object.emplace_back("sha256", string(asset.sha256));
  object.emplace_back("size_bytes", integer(static_cast<int64_t>(asset.size_bytes)));
  object.emplace_back("cache_seconds", integer(asset.cache_seconds));
  return JsonValue::makeObject(std::move(object));
}

JsonValue sampleToJson(const MetricSample& sample) {
  JsonValue::Object object;
  object.emplace_back("source_id", string(sample.source_id));
  object.emplace_back("metric", string(sample.metric));
  object.emplace_back("timestamp_ms", integer(sample.timestamp_ms));
  object.emplace_back("value", number(sample.value));
  object.emplace_back("unit", string(sample.unit));
  object.emplace_back("labels", keyValuesToJson(sample.labels));
  return JsonValue::makeObject(std::move(object));
}

JsonValue eventToJson(const Event& event) {
  JsonValue::Object object;
  object.emplace_back("id", string(event.id));
  object.emplace_back("rule_id", string(event.rule_id));
  object.emplace_back("source_id", string(event.source_id));
  object.emplace_back("timestamp_ms", integer(event.timestamp_ms));
  object.emplace_back("severity", string(severityName(event.severity)));
  object.emplace_back("state", string(eventStateName(event.state)));
  object.emplace_back("title", string(event.title));
  object.emplace_back("message", string(event.message));
  object.emplace_back("linked_panel_id", string(event.linked_panel_id));
  return JsonValue::makeObject(std::move(object));
}

}  // namespace

JsonValue dashboardToJson(const Dashboard& dashboard) {
  JsonValue::Object root;
  root.emplace_back("schema", string(kSchemaName));
  root.emplace_back("schema_version", integer(kSchemaVersion));
  root.emplace_back("id", string(dashboard.id));
  root.emplace_back("title", string(dashboard.title));
  root.emplace_back("default_page_id", string(dashboard.default_page_id));
  root.emplace_back("refresh_ms", integer(dashboard.refresh_ms));
  JsonValue::Array sources;
  for (const auto& source : dashboard.sources) sources.push_back(sourceToJson(source));
  root.emplace_back("sources", JsonValue::makeArray(std::move(sources)));
  JsonValue::Array variables;
  for (const auto& variable : dashboard.variables) variables.push_back(variableToJson(variable));
  root.emplace_back("variables", JsonValue::makeArray(std::move(variables)));
  JsonValue::Array pages;
  for (const auto& page : dashboard.pages) {
    JsonValue::Object object;
    object.emplace_back("id", string(page.id));
    object.emplace_back("title", string(page.title));
    object.emplace_back("visible", boolean(page.visible));
    object.emplace_back("grid_columns", integer(page.grid_columns));
    object.emplace_back("row_height", integer(page.row_height));
    JsonValue::Array panels;
    for (const auto& panel : page.panels) panels.push_back(panelToJson(panel));
    object.emplace_back("panels", JsonValue::makeArray(std::move(panels)));
    pages.push_back(JsonValue::makeObject(std::move(object)));
  }
  root.emplace_back("pages", JsonValue::makeArray(std::move(pages)));
  JsonValue::Array rules;
  for (const auto& rule : dashboard.alert_rules) rules.push_back(alertRuleToJson(rule));
  root.emplace_back("alert_rules", JsonValue::makeArray(std::move(rules)));
  JsonValue::Array assets;
  for (const auto& asset : dashboard.assets) assets.push_back(assetToJson(asset));
  root.emplace_back("assets", JsonValue::makeArray(std::move(assets)));
  JsonValue::Array samples;
  for (const auto& sample : dashboard.samples) samples.push_back(sampleToJson(sample));
  root.emplace_back("samples", JsonValue::makeArray(std::move(samples)));
  JsonValue::Array events;
  for (const auto& event : dashboard.events) events.push_back(eventToJson(event));
  root.emplace_back("events", JsonValue::makeArray(std::move(events)));
  return JsonValue::makeObject(std::move(root));
}

}  // namespace eego::dashboard
