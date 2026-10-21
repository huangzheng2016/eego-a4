// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dashboard_core/expression.h"
#include "dashboard_core/model.h"
#include "data_pipeline/history.h"

namespace panel {

struct DashboardCarouselRoute {
  bool allowed = false;
  std::string owner_id;
  std::string source_id;
  std::string metric;
};

inline DashboardCarouselRoute dashboardCarouselRoute(
    const std::string& owner_id, const std::string& selected_id,
    const std::string& selected_source, const std::string& selected_metric,
    const std::string& update_panel, const std::string& update_source) {
  DashboardCarouselRoute route{false, owner_id, selected_source, selected_metric};
  route.allowed = update_source == selected_source && update_panel == selected_id;
  return route;
}

inline std::string dashboardResolveCarousel(
    const std::map<std::string, std::string>& selections, std::string panel_id) {
  std::set<std::string> visited;
  while (true) {
    if (!visited.insert(panel_id).second) return panel_id;
    const auto selected = selections.find(panel_id);
    if (selected == selections.end() || selected->second == panel_id) return panel_id;
    panel_id = selected->second;
  }
}

inline bool dashboardAggregationForExpression(std::string_view expression,
                                              eego::dashboard::pipeline::Aggregation& out) {
  while (!expression.empty() && expression.front() == ' ') expression.remove_prefix(1);
  while (!expression.empty() && expression.back() == ' ') expression.remove_suffix(1);
  if (expression == "sum") {
    out = eego::dashboard::pipeline::Aggregation::Sum;
  } else if (expression == "avg") {
    out = eego::dashboard::pipeline::Aggregation::Avg;
  } else if (expression == "min") {
    out = eego::dashboard::pipeline::Aggregation::Min;
  } else if (expression == "max") {
    out = eego::dashboard::pipeline::Aggregation::Max;
  } else if (expression == "delta") {
    out = eego::dashboard::pipeline::Aggregation::Delta;
  } else if (expression == "rate") {
    out = eego::dashboard::pipeline::Aggregation::Rate;
  } else {
    return false;
  }
  return true;
}

inline eego::dashboard::pipeline::WindowKind dashboardWindowFromPanel(
    const eego::dashboard::PanelSpec& panel) {
  if (panel.time_window.from == "today") return eego::dashboard::pipeline::WindowKind::Today;
  if (panel.time_window.from == "-5h") return eego::dashboard::pipeline::WindowKind::Fixed5h;
  if (panel.time_window.from == "-7d") return eego::dashboard::pipeline::WindowKind::Fixed7d;
  return eego::dashboard::pipeline::WindowKind::Rolling24h;
}

inline bool dashboardPanelHasCustomWindow(const eego::dashboard::PanelSpec& panel) {
  return panel.time_window.to != "now" ||
         (panel.time_window.from != "today" && panel.time_window.from != "-5h" &&
          panel.time_window.from != "-7d");
}

inline eego::dashboard::pipeline::ResolvedWindow dashboardResolvePanelWindow(
    const eego::dashboard::PanelSpec& panel, int64_t now_ms) {
  eego::dashboard::pipeline::ResolvedWindow resolved =
      eego::dashboard::pipeline::resolveWindow(dashboardWindowFromPanel(panel), now_ms);
  int64_t endpoint = 0;
  if (eego::dashboard::parseTimeExpression(panel.time_window.from, now_ms, endpoint))
    resolved.from_ms = endpoint;
  if (eego::dashboard::parseTimeExpression(panel.time_window.to, now_ms, endpoint))
    resolved.to_ms = endpoint;
  return {std::min(resolved.from_ms, resolved.to_ms),
          std::max(resolved.from_ms, resolved.to_ms)};
}

inline eego::dashboard::MetricSample dashboardAggregateSample(
    const eego::dashboard::PanelSpec& panel,
    const eego::dashboard::pipeline::AggregationResult& aggregate, int64_t now_ms) {
  eego::dashboard::MetricSample sample;
  sample.source_id = panel.source_id;
  sample.metric = panel.metric;
  sample.timestamp_ms = aggregate.last_timestamp_ms != 0 ? aggregate.last_timestamp_ms : now_ms;
  sample.value = aggregate.value;
  sample.unit = panel.style.unit;
  return sample;
}

inline bool dashboardEvaluateValueExpression(const eego::dashboard::PanelSpec& panel,
                                             eego::dashboard::MetricSample& sample) {
  if (panel.value_expression.empty()) return true;
  eego::dashboard::pipeline::Aggregation aggregate;
  if (dashboardAggregationForExpression(panel.value_expression, aggregate)) return true;
  eego::dashboard::EvalValues values{
      {"value", eego::dashboard::EvalValue::makeNumber(sample.value)},
      {"now", eego::dashboard::EvalValue::makeNumber(static_cast<double>(sample.timestamp_ms))},
      {"timestamp_ms",
       eego::dashboard::EvalValue::makeNumber(static_cast<double>(sample.timestamp_ms))}};
  double transformed = 0.0;
  eego::dashboard::ExpressionError error;
  if (!eego::dashboard::evaluateNumericExpression(panel.value_expression, values, transformed,
                                                  error)) {
    return false;
  }
  sample.value = transformed;
  return true;
}

inline bool dashboardPanelRequiresPublication(const eego::dashboard::PanelSpec& panel) {
  return !panel.source_id.empty() && !panel.metric.empty() &&
         (panel.kind == eego::dashboard::PanelKind::Card ||
          panel.kind == eego::dashboard::PanelKind::Table ||
          panel.kind == eego::dashboard::PanelKind::Chart ||
          panel.kind == eego::dashboard::PanelKind::Image);
}

inline std::vector<std::string> dashboardQueryValues(
    const eego::dashboard::Variable& variable,
    const std::vector<eego::dashboard::MetricSample>& samples) {
  std::vector<std::string> out;
  if (variable.kind != eego::dashboard::VariableKind::Query) return out;
  auto add = [&](const std::string& value) {
    if (out.size() < 64 && std::find(out.begin(), out.end(), value) == out.end())
      out.push_back(value);
  };
  std::string_view path(variable.value_path);
  if (path.substr(0, 2) == "$.") path.remove_prefix(2);
  if (path.substr(0, 7) == "labels.") path.remove_prefix(7);
  const size_t separator = path.find_last_of("./");
  if (separator != std::string_view::npos) path.remove_prefix(separator + 1);
  for (const eego::dashboard::MetricSample& sample : samples) {
    if (sample.source_id != variable.source_id) continue;
    if (path == "metric") {
      add(sample.metric);
    } else if (path == "source" || path == "source_id") {
      add(sample.source_id);
    } else {
      for (const eego::dashboard::KeyValue& label : sample.labels) {
        if (label.key == path) add(label.value);
      }
    }
  }
  return out;
}

class DashboardPublicationStore {
 public:
  struct Publication {
    std::string metric;
    std::vector<eego::dashboard::MetricSample> samples;
  };

  static std::string alias(const std::string& panel_id) { return "__panel_" + panel_id; }

  bool contains(const std::string& panel_id) const { return publications_.count(panel_id) != 0; }
  const Publication& at(const std::string& panel_id) const { return publications_.at(panel_id); }

  std::string metric(const std::string& panel_id, const std::string& fallback) const {
    const auto found = publications_.find(panel_id);
    return found == publications_.end() ? fallback : found->second.metric;
  }

  void publish(const std::string& panel_id, const std::string& metric,
               std::vector<eego::dashboard::MetricSample> samples) {
    publications_[panel_id] = {metric, std::move(samples)};
  }

  void erase(const std::string& panel_id) { publications_.erase(panel_id); }
  void clear() { publications_.clear(); }

 private:
  std::map<std::string, Publication> publications_;
};

}  // namespace panel
