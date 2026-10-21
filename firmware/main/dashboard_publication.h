// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dashboard_core/model.h"

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
  route.allowed = update_source == selected_source &&
                  (update_panel == owner_id || update_panel == selected_id);
  return route;
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
