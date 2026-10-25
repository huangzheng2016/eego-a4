// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "dashboard_core/json.h"
#include "dashboard_core/model.h"

namespace eego::dashboard {

// Canonical v2 budgets. Callers may only tighten these values; larger custom
// profiles are rejected so export always remains valid for default import.
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

// Repairs a stored dashboard JSON object (not a bundle) written before the
// screen layout rule (kGridRowsPerScreen) was enforced: visible panels that are
// taller than a screen are shortened, and panels that cross a screen boundary
// or overlap an earlier visible panel are moved to the first free spot at or
// after their original row. Valid layouts are left untouched. Returns the
// number of panels changed. Used only on load so older SD configurations keep
// working; new submissions are validated strictly instead.
size_t repairLegacyLayout(JsonValue& dashboard);
// Same as repairLegacyLayout, applied to a stored document that is either a
// dashboard bundle (repairs its "dashboard" member) or a bare dashboard object.
// Shared by the admin store (which persists the result) and the runtime loader
// (which repairs defensively before strict validation).
size_t repairLegacyStoredLayout(JsonValue& stored);
bool exportDashboard(const Dashboard& dashboard, std::string& out,
                     std::vector<ValidationError>& errors,
                     const Limits& limits = {});

}  // namespace eego::dashboard
