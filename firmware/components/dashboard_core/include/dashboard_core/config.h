// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "dashboard_core/json.h"
#include "dashboard_core/model.h"

namespace eego::dashboard {

struct Limits {
  size_t max_json_bytes = 128 * 1024;
  size_t max_depth = 16;
  size_t max_sources = 12;
  size_t max_variables = 24;
  size_t max_variable_options = 64;
  size_t max_pages = 12;
  size_t max_panels_per_page = 24;
  size_t max_total_panels = 72;
  size_t max_actions_per_panel = 8;
  size_t max_thresholds_per_panel = 16;
  size_t max_links_per_panel = 16;
  size_t max_columns_per_panel = 24;
  size_t max_alert_rules = 48;
  size_t max_assets = 24;
  size_t max_samples = 2048;
  size_t max_events = 256;
};

struct ValidationError {
  std::string field;
  std::string message;
};

struct ParseOptions {
  Limits limits;
  bool reject_unknown_fields = true;
};

struct ParseResult {
  Dashboard dashboard;
  std::vector<ValidationError> errors;
  uint32_t source_version = 0;
  bool migrated = false;

  bool ok() const { return errors.empty(); }
};

bool validateLimits(const Limits& limits, ValidationError& err);
ParseResult parseDashboard(std::string_view json, const ParseOptions& options = {});
ParseResult dashboardFromJson(const JsonValue& root, const ParseOptions& options = {});

bool validateDashboard(const Dashboard& dashboard, const Limits& limits,
                       std::vector<ValidationError>& errors);
JsonValue dashboardToJson(const Dashboard& dashboard);
bool exportDashboard(const Dashboard& dashboard, std::string& out,
                     std::vector<ValidationError>& errors,
                     const Limits& limits = {});

}  // namespace eego::dashboard
