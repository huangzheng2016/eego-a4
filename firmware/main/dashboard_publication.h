// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "dashboard_core/model.h"

namespace panel {

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
