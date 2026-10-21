// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "data_pipeline/cache.h"
#include "data_pipeline/source_plan.h"

namespace eego::dashboard::pipeline {

struct PipelineConfig {
  HistoryLimits history;
  VariableValues variable_overrides;
  int32_t timezone_offset_minutes = 0;
  uint32_t default_panel_refresh_ms = 60000;
  TokenResolver token_resolver;
};

struct PanelBinding {
  std::string panel_id;
  std::string source_id;
  std::string metric;
  std::vector<KeyValue> labels;
  WindowKind window = WindowKind::Rolling24h;
  Aggregation aggregation = Aggregation::Avg;
  bool has_aggregation = false;
};

struct PanelUpdate {
  std::string panel_id;
  std::string source_id;
  std::shared_ptr<const SourceResult> result;
  AggregationResult aggregate;
  SourceStatus status;
};

struct TickResult {
  size_t due_sources = 0;
  size_t http_requests = 0;
  size_t deduplicated_requests = 0;
  size_t succeeded = 0;
  size_t failed = 0;
  size_t dirty_panels = 0;
};

class DataPipeline {
 public:
  explicit DataPipeline(IHttpTransport& transport, PipelineConfig config = {});
  ~DataPipeline();

  // plans may be empty: dashboard HTTP sources then use StandardJson defaults.
  // Pipeline state is single-owner; async transports pump requests and deliver
  // results without blocking the caller, while HTTP still executes in net_task.
  bool configure(const Dashboard& dashboard, const std::vector<SourcePlan>& plans,
                 SdCache* cache, std::string& error);
  bool setVariables(const VariableValues& overrides, std::string& error);
  bool setPanelBinding(const PanelBinding& binding, std::string& error);
  bool setPanelRefreshOverride(const std::string& panel_id, uint32_t refresh_ms,
                               std::string& error);

  TickResult tick(int64_t now_ms);
  bool asyncIdle() const;
  bool refreshSource(const std::string& source_id);
  bool refreshPanel(const std::string& panel_id);

  std::shared_ptr<const SourceResult> sourceResult(const std::string& source_id) const;
  bool sourceStatus(const std::string& source_id, SourceStatus& out) const;
  int64_t nextWakeMs(int64_t now_ms) const;

  const MetricHistory& history() const { return history_; }
  const std::vector<PanelUpdate>& updates() const { return updates_; }
  void clearUpdates() { updates_.clear(); }
  std::vector<std::string> takeDirtyPanels();

 private:
  struct SourceRuntime;

  struct PanelState {
    PanelBinding binding;
    uint32_t refresh_ms = 60000;
    uint32_t override_ms = 0;
    int64_t next_refresh_ms = 0;
    bool pending = true;
  };

  struct AsyncBatch {
    uint64_t id = 0;
    std::string key;
    std::vector<std::string> source_ids;
  };

  IHttpTransport& transport_;
  PipelineConfig config_;
  Dashboard dashboard_;
  VariableValues variables_;
  MetricHistory history_;
  SdCache* cache_ = nullptr;
  std::map<std::string, std::unique_ptr<SourceRuntime>> sources_;
  std::map<std::string, PanelState> panels_;
  std::vector<PanelUpdate> updates_;
  std::set<std::string> dirty_panels_;
  std::map<uint64_t, AsyncBatch> async_batches_;
  std::map<std::string, uint64_t> async_request_keys_;

  uint32_t panelInterval(const PanelState& panel) const;
  uint32_t sourceInterval(const SourceRuntime& source) const;
  void markDirtyPanels(const SourceRuntime& source, const SourceResult* previous,
                       const SourceResult& current, int64_t now_ms, bool status_changed);
  void markStatusPanels(const SourceRuntime& source, int64_t now_ms);
  void servicePanels(const SourceRuntime& source, int64_t now_ms);
  PanelUpdate makeUpdate(const PanelState& panel, const SourceRuntime& source,
                         int64_t now_ms) const;
  TickResult tickAsync(int64_t now_ms);
  void drainAsync(int64_t now_ms, TickResult& tick);
  void completeAsyncSource(SourceRuntime& source, const HttpResponse& response,
                           int64_t now_ms, TickResult& tick);
  void failSource(SourceRuntime& source, const std::string& failure, int64_t now_ms,
                  TickResult& tick);
  void succeedSource(SourceRuntime& source, std::shared_ptr<const SourceResult> current,
                     bool not_modified, int64_t now_ms, TickResult& tick);
  void detachAsync(SourceRuntime& source);
  void detachAllAsync();
};

}  // namespace eego::dashboard::pipeline
