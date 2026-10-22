// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dashboard_core/config.h"
#include "data_pipeline/source_plan.h"
#include "esp_err.h"

namespace eego::admin {

inline constexpr char kDashboardBundleSchema[] = "eego.dashboard-bundle/1";
inline constexpr uint32_t kDashboardBundleVersion = 1;
inline constexpr size_t kDashboardBundleMaxBytes = 192 * 1024;
inline constexpr size_t kDashboardSourcePlanMaxBytes = 32 * 1024;

// One immutable, atomically replaceable configuration. source_plan_values retains
// the validated declarative JSON used for export; plans is the runtime view.
struct DashboardDocument {
  dashboard::Dashboard dashboard;
  std::vector<dashboard::pipeline::SourcePlan> plans;
  dashboard::JsonValue source_plan_values = dashboard::JsonValue::makeArray({});
  uint64_t revision = 0;
  bool configured = false;
};

struct DashboardParseResult {
  DashboardDocument document;
  std::vector<dashboard::ValidationError> errors;
  uint32_t source_version = 0;
  bool migrated = false;

  bool ok() const { return errors.empty(); }
};

class DashboardStore {
 public:
  static DashboardStore& instance();

  // Missing files are an unconfigured, valid store. Invalid files are backed up
  // to dashboard.json.bak and never exposed as a partially parsed configuration.
  esp_err_t init(const std::string& path);
  bool valid() const;
  const std::string& path() const { return path_; }

  std::shared_ptr<const DashboardDocument> snapshot() const;

  // Accepts a bundle or a raw v1/v2 dashboard. Plain sensitive headers are rejected;
  // credential refs remain metadata and are never resolved into serialized output.
  // The input revision is informational; commit() assigns the next authoritative revision.
  static DashboardParseResult parse(const std::string& json,
                                    const DashboardDocument* current = nullptr);
  static bool validate(DashboardDocument& candidate, const DashboardDocument* current,
                       std::vector<dashboard::ValidationError>& errors);

  static dashboard::JsonValue toJson(const DashboardDocument& document,
                                     bool mask_secrets = true,
                                     bool dashboard_only = false);
  static const char* sampleJson();

  // app_task-only SD writers. The whole candidate is revalidated before one
  // tmp+rename+readback transaction. Revision conflicts leave disk and RAM untouched.
  bool commit(const DashboardDocument& candidate, bool check_revision,
              uint64_t expected_revision, std::string& error, bool& conflict);
  bool erase(bool check_revision, uint64_t expected_revision,
             std::string& error, bool& conflict);

 private:
  DashboardStore() = default;

  mutable void* mutex_ = nullptr;  // SemaphoreHandle_t
  std::string path_;
  std::shared_ptr<const DashboardDocument> snapshot_;
  bool valid_ = true;
};

}  // namespace eego::admin
