// SPDX-License-Identifier: GPL-3.0-or-later
#include "data_pipeline/pipeline.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>

#include "data_pipeline/parser.h"

namespace eego::dashboard::pipeline {
namespace {

std::atomic<uint64_t> g_next_async_id{1};

uint64_t nextAsyncId() { return g_next_async_id.fetch_add(1); }

void appendPart(std::string& out, std::string_view value) {
  out += std::to_string(value.size());
  out.push_back(':');
  out.append(value);
  out.push_back(';');
}

std::string sampleSignature(const MetricSample& sample) {
  std::string out;
  appendPart(out, sample.metric);
  appendPart(out, std::to_string(sample.timestamp_ms));
  char number[40];
  std::snprintf(number, sizeof(number), "%.17g", sample.value);
  appendPart(out, number);
  appendPart(out, sample.unit);
  std::vector<KeyValue> labels = sample.labels;
  std::sort(labels.begin(), labels.end(), [](const KeyValue& left, const KeyValue& right) {
    return left.key == right.key ? left.value < right.value : left.key < right.key;
  });
  for (const KeyValue& label : labels) {
    appendPart(out, label.key);
    appendPart(out, label.value);
  }
  return out;
}

std::string eventSignature(const Event& event) {
  std::string out;
  appendPart(out, event.id);
  appendPart(out, event.rule_id);
  appendPart(out, std::to_string(event.timestamp_ms));
  appendPart(out, severityName(event.severity));
  appendPart(out, eventStateName(event.state));
  appendPart(out, event.title);
  appendPart(out, event.message);
  appendPart(out, event.linked_panel_id);
  return out;
}

struct ChangeSet {
  bool any = false;
  std::set<std::string> metrics;
};

ChangeSet diffResults(const SourceResult* previous, const SourceResult& current) {
  ChangeSet changes;
  if (previous == nullptr) {
    changes.any = true;
    for (const MetricSample& sample : current.metrics) changes.metrics.insert(sample.metric);
    return changes;
  }
  std::map<std::string, std::vector<std::string>> old_metrics;
  std::map<std::string, std::vector<std::string>> new_metrics;
  for (const MetricSample& sample : previous->metrics)
    old_metrics[sample.metric].push_back(sampleSignature(sample));
  for (const MetricSample& sample : current.metrics)
    new_metrics[sample.metric].push_back(sampleSignature(sample));
  for (auto& item : old_metrics) std::sort(item.second.begin(), item.second.end());
  for (auto& item : new_metrics) std::sort(item.second.begin(), item.second.end());
  std::set<std::string> metric_names;
  for (const auto& item : old_metrics) metric_names.insert(item.first);
  for (const auto& item : new_metrics) metric_names.insert(item.first);
  for (const std::string& metric : metric_names) {
    if (old_metrics[metric] != new_metrics[metric]) changes.metrics.insert(metric);
  }
  std::vector<std::string> old_events;
  std::vector<std::string> new_events;
  for (const Event& event : previous->events) old_events.push_back(eventSignature(event));
  for (const Event& event : current.events) new_events.push_back(eventSignature(event));
  std::sort(old_events.begin(), old_events.end());
  std::sort(new_events.begin(), new_events.end());
  const bool text_changed = previous->has_text != current.has_text ||
                            (current.has_text && previous->text.text != current.text.text);
  const bool image_changed = previous->has_image != current.has_image ||
                             (current.has_image &&
                              (previous->image.media_type != current.image.media_type ||
                               previous->image.bytes != current.image.bytes));
  const bool reset_changed = previous->reset_at_ms != current.reset_at_ms;
  changes.any = !changes.metrics.empty() || old_events != new_events || text_changed ||
                image_changed || reset_changed;
  if (reset_changed) {
    for (const auto& reset : current.reset_at_ms) {
      if (reset.first == "*")
        changes.any = true;
      else
        changes.metrics.insert(reset.first);
    }
    if (changes.metrics.empty()) changes.any = true;
  }
  return changes;
}

WindowKind windowFromPanel(const PanelSpec& panel) {
  if (panel.time_window.from == "today") return WindowKind::Today;
  if (panel.time_window.from == "-5h") return WindowKind::Fixed5h;
  if (panel.time_window.from == "-7d") return WindowKind::Fixed7d;
  return WindowKind::Rolling24h;
}

bool validRefreshOverride(uint32_t refresh_ms) {
  return refresh_ms == 0 || refresh_ms == 60000 || refresh_ms == 300000 ||
         refresh_ms == 900000 || refresh_ms == 1800000;
}

}  // namespace

struct DataPipeline::SourceRuntime {
  SourcePlan plan;
  std::shared_ptr<const SourceResult> result;
  std::shared_ptr<const SourceResult> static_result;
  SourceStatus status;
  int64_t next_poll_ms = 0;
  bool initial_fanout = true;
  bool async_pending = false;
  uint64_t async_id = 0;
  int64_t async_deadline_ms = 0;
};

DataPipeline::~DataPipeline() = default;

DataPipeline::DataPipeline(IHttpTransport& transport, PipelineConfig config)
    : transport_(transport),
      config_(std::move(config)),
      history_(config_.history, config_.timezone_offset_minutes) {}

bool DataPipeline::configure(const Dashboard& dashboard, const std::vector<SourcePlan>& plans,
                             SdCache* cache, std::string& error) {
  error.clear();
  VariableValues variables;
  if (!defaultVariableValues(dashboard.variables, config_.variable_overrides, variables, error))
    return false;

  dashboard_ = dashboard;
  cache_ = cache;
  if (cache_ != nullptr) cache_->setTimezoneOffset(config_.timezone_offset_minutes);
  variables_ = std::move(variables);
  history_ = MetricHistory(config_.history, config_.timezone_offset_minutes);
  sources_.clear();
  panels_.clear();
  updates_.clear();
  dirty_panels_.clear();
  async_batches_.clear();
  async_request_keys_.clear();
  async_suspended_ = false;

  for (const SourcePlan& plan : plans) {
    if (!validateSourcePlan(plan, error)) return false;
    if (sources_.count(plan.source.id) != 0) {
      error = "duplicate source plan: " + plan.source.id;
      return false;
    }
    auto runtime = std::make_unique<SourceRuntime>();
    runtime->plan = plan;
    runtime->status.source_id = plan.source.id;
    sources_.emplace(plan.source.id, std::move(runtime));
  }
  for (const Source& source : dashboard.sources) {
    if (sources_.count(source.id) != 0) continue;
    SourcePlan plan;
    plan.source = source;
    auto runtime = std::make_unique<SourceRuntime>();
    runtime->plan = std::move(plan);
    runtime->status.source_id = source.id;
    if (source.kind == SourceKind::Static) {
      auto result = std::make_shared<SourceResult>();
      result->source_id = source.id;
      for (const MetricSample& sample : dashboard.samples) {
        if (sample.source_id == source.id) result->metrics.push_back(sample);
      }
      for (const Event& event : dashboard.events) {
        if (event.source_id == source.id) result->events.push_back(event);
      }
      runtime->static_result = std::move(result);
    }
    sources_.emplace(source.id, std::move(runtime));
  }

  for (auto& item : sources_) {
    SourceRuntime& runtime = *item.second;
    if (cache_ != nullptr) {
      std::string cache_error;
      std::vector<HistoryEntry> entries;
      if (cache_->loadHistory(item.first, entries, cache_error))
        history_.replaceSource(item.first, entries, 0, cache_error);
      SourceResult cached;
      if (cache_->readSnapshot(item.first, cached, cache_error)) {
        runtime.result = std::make_shared<const SourceResult>(std::move(cached));
        history_.reconcileReset(*runtime.result);
        runtime.status.last_success_ms = runtime.result->fetched_at_ms;
        runtime.status.offline = true;
        runtime.status.stale = true;
      }
    }
  }

  std::set<std::string> linked_dependencies;
  for (const Page& page : dashboard.pages) {
    for (const PanelSpec& parent : page.panels) {
      if (!parent.visible) continue;
      linked_dependencies.insert(parent.linked_panel_ids.begin(), parent.linked_panel_ids.end());
    }
  }
  for (const Page& page : dashboard.pages) {
    for (const PanelSpec& spec : page.panels) {
      if (spec.source_id.empty() || (!spec.visible && linked_dependencies.count(spec.id) == 0))
        continue;
      if (panels_.count(spec.id) != 0) {
        error = "duplicate data panel: " + spec.id;
        return false;
      }
      if (sources_.count(spec.source_id) == 0) {
        error = "panel references an unknown source: " + spec.id;
        return false;
      }
      PanelState panel;
      panel.binding.panel_id = spec.id;
      panel.binding.source_id = spec.source_id;
      panel.binding.metric = spec.metric;
      panel.binding.window = windowFromPanel(spec);
      panel.refresh_ms = spec.refresh_ms != 0 ? spec.refresh_ms : config_.default_panel_refresh_ms;
      panels_.emplace(spec.id, std::move(panel));
    }
  }
  return true;
}

bool DataPipeline::setVariables(const VariableValues& overrides, std::string& error) {
  VariableValues values;
  if (!defaultVariableValues(dashboard_.variables, overrides, values, error)) return false;
  config_.variable_overrides = overrides;
  variables_ = std::move(values);
  detachAllAsync();
  for (auto& source : sources_) source.second->next_poll_ms = 0;
  return true;
}

bool DataPipeline::setPanelBinding(const PanelBinding& binding, std::string& error) {
  const auto panel = panels_.find(binding.panel_id);
  if (panel == panels_.end()) {
    error = "unknown panel: " + binding.panel_id;
    return false;
  }
  if (sources_.count(binding.source_id) == 0 ||
      (binding.has_aggregation && binding.metric.empty()) || binding.labels.size() > kMaxLabels) {
    error = "invalid panel source, metric, or label binding";
    return false;
  }
  panel->second.binding = binding;
  panel->second.pending = true;
  panel->second.next_refresh_ms = 0;
  sources_[binding.source_id]->next_poll_ms = 0;
  return true;
}

bool DataPipeline::setPanelRefreshOverride(const std::string& panel_id, uint32_t refresh_ms,
                                           std::string& error) {
  const auto panel = panels_.find(panel_id);
  if (panel == panels_.end()) {
    error = "unknown panel: " + panel_id;
    return false;
  }
  if (!validRefreshOverride(refresh_ms)) {
    error = "panel refresh override must be 0, 1m, 5m, 15m, or 30m";
    return false;
  }
  panel->second.override_ms = refresh_ms;
  panel->second.pending = true;
  panel->second.next_refresh_ms = 0;
  sources_[panel->second.binding.source_id]->next_poll_ms = 0;
  return true;
}

uint32_t DataPipeline::panelInterval(const PanelState& panel) const {
  const uint32_t interval = panel.override_ms != 0 ? panel.override_ms : panel.refresh_ms;
  return std::max<uint32_t>(interval, 60000);
}

uint32_t DataPipeline::sourceInterval(const SourceRuntime& source) const {
  uint32_t interval = source.plan.source.refresh_ms;
  for (const auto& item : panels_) {
    if (item.second.binding.source_id == source.plan.source.id)
      interval = std::min(interval, panelInterval(item.second));
  }
  return std::max<uint32_t>(interval, 1000);
}

PanelUpdate DataPipeline::makeUpdate(const PanelState& panel, const SourceRuntime& source,
                                     int64_t now_ms) const {
  PanelUpdate update;
  update.panel_id = panel.binding.panel_id;
  update.source_id = source.plan.source.id;
  update.result = source.result;
  update.status = source.status;
  if (panel.binding.has_aggregation) {
    update.aggregate = history_.aggregate(panel.binding.source_id, panel.binding.metric,
                                          panel.binding.labels, panel.binding.window, now_ms,
                                          panel.binding.aggregation,
                                          config_.timezone_offset_minutes);
  }
  return update;
}

void DataPipeline::markDirtyPanels(const SourceRuntime& source, const SourceResult* previous,
                                   const SourceResult& current, int64_t now_ms,
                                   bool status_changed) {
  (void)now_ms;
  const ChangeSet changes = diffResults(previous, current);
  for (auto& item : panels_) {
    PanelState& panel = item.second;
    if (panel.binding.source_id != source.plan.source.id) continue;
    const bool metric_changed = !panel.binding.metric.empty() &&
                                changes.metrics.count(panel.binding.metric) != 0;
    const bool relevant = status_changed || metric_changed ||
                          (panel.binding.metric.empty() && changes.any);
    if (relevant) panel.pending = true;
  }
}

void DataPipeline::markStatusPanels(const SourceRuntime& source, int64_t now_ms) {
  (void)now_ms;
  for (auto& item : panels_) {
    if (item.second.binding.source_id == source.plan.source.id) item.second.pending = true;
  }
}

void DataPipeline::servicePanels(const SourceRuntime& source, int64_t now_ms) {
  for (auto& item : panels_) {
    PanelState& panel = item.second;
    if (panel.binding.source_id != source.plan.source.id || now_ms < panel.next_refresh_ms)
      continue;
    panel.next_refresh_ms = now_ms + panelInterval(panel);
    if (!panel.pending && !panel.binding.has_aggregation) continue;
    PanelUpdate update = makeUpdate(panel, source, now_ms);
    const auto existing =
        std::find_if(updates_.begin(), updates_.end(), [&](const PanelUpdate& old) {
      return old.panel_id == update.panel_id;
    });
    if (existing == updates_.end())
      updates_.push_back(std::move(update));
    else
      *existing = std::move(update);
    dirty_panels_.insert(panel.binding.panel_id);
    panel.pending = false;
  }
}

TickResult DataPipeline::tick(int64_t now_ms) {
  if (transport_.asyncSupported()) return tickAsync(now_ms);
  TickResult tick;
  std::map<std::string, std::shared_ptr<HttpResponse>> responses;
  for (auto& item : sources_) {
    SourceRuntime& source = *item.second;
    SourceStatus& status = source.status;
    const bool was_stale = status.stale;
    if (source.result) {
      status.stale = source.plan.stale_after_ms != 0 &&
                     (status.last_success_ms <= 0 ||
                      (now_ms >= status.last_success_ms &&
                       now_ms - status.last_success_ms > source.plan.stale_after_ms));
    } else {
      status.stale = true;
    }
    if (status.stale != was_stale) markStatusPanels(source, now_ms);
    if (!source.plan.source.enabled || now_ms < source.next_poll_ms) continue;

    ++tick.due_sources;
    const bool initial = source.initial_fanout;
    const bool offline_before = status.offline;
    const bool stale_before = status.stale;
    status.last_attempt_ms = now_ms;
    std::shared_ptr<const SourceResult> previous = source.result;
    std::shared_ptr<const SourceResult> current;
    std::string failure;
    bool not_modified = false;

    if (source.plan.source.kind == SourceKind::Static) {
      status.last_http_status = 200;
      if (source.static_result) {
        auto copy = std::make_shared<SourceResult>(*source.static_result);
        copy->fetched_at_ms = now_ms;
        current = std::move(copy);
      } else {
        HttpResponse response;
        response.status = 200;
        if (!interpolateVariables(source.plan.static_body, variables_, response.body, failure,
                                  source.plan.source.max_response_bytes)) {
          // failure already contains the interpolation error.
        } else {
          SourceResult parsed;
          if (parseSourceResponse(source.plan, response, variables_, now_ms, parsed, failure))
            current = std::make_shared<const SourceResult>(std::move(parsed));
        }
      }
    } else {
      net::NetRequest request;
      if (!buildSourceRequest(source.plan, variables_, config_.token_resolver, request, failure)) {
        status.last_http_status = 0;
      } else {
        const std::string key = sourceRequestKey(request);
        std::shared_ptr<HttpResponse> response;
        const auto cached = responses.find(key);
        if (cached != responses.end()) {
          response = cached->second;
          ++tick.deduplicated_requests;
        } else {
          response = std::make_shared<HttpResponse>();
          ++tick.http_requests;
          if (!transport_.execute(request, *response) && response->error.empty())
            response->error = "HTTP transport unavailable";
          responses.emplace(key, response);
        }
        status.last_http_status = response->status;
        not_modified = response->not_modified || response->status == 304;
        if (not_modified) {
          if (!previous) {
            failure = "304 response has no cached data";
          } else {
            auto copy = std::make_shared<SourceResult>(*previous);
            copy->fetched_at_ms = now_ms;
            current = std::move(copy);
          }
        } else {
          SourceResult parsed;
          if (parseSourceResponse(source.plan, *response, variables_, now_ms, parsed, failure))
            current = std::make_shared<const SourceResult>(std::move(parsed));
        }
      }
    }

    if (!failure.empty() || !current) {
      if (failure.empty()) failure = "source produced no result";
      status.error = failure;
      status.offline = true;
      status.consecutive_failures = std::min<uint32_t>(status.consecutive_failures + 1, 1000000);
      if (!source.result)
        status.stale = true;
      else if (source.plan.stale_after_ms == 0)
        status.stale = false;
      const bool status_changed = initial || status.offline != offline_before ||
                                  status.stale != stale_before;
      if (status_changed) markStatusPanels(source, now_ms);
      const uint32_t shift = std::min<uint32_t>(status.consecutive_failures, 6);
      const uint64_t backoff = std::min<uint64_t>(
          static_cast<uint64_t>(sourceInterval(source)) << shift, 1800000ULL);
      source.next_poll_ms = now_ms + static_cast<int64_t>(backoff);
      source.initial_fanout = false;
      ++tick.failed;
      continue;
    }

    source.result = current;
    status.last_success_ms = now_ms;
    status.offline = false;
    status.stale = false;
    status.error.clear();
    status.cache_error.clear();
    status.consecutive_failures = 0;
    if (!not_modified) {
      std::string history_error;
      if (!history_.append(*current, now_ms, history_error))
        status.cache_error = "history: " + history_error;
      if (cache_ != nullptr) {
        std::vector<HistoryEntry> entries;
        entries.reserve(current->metrics.size());
        for (const MetricSample& sample : current->metrics) {
          HistoryEntry entry;
          entry.sample = sample;
          entry.reset_at_ms = current->resetAtFor(sample.metric);
          entries.push_back(std::move(entry));
        }
        std::string cache_error;
        if (!entries.empty() && !cache_->appendHistory(current->source_id, entries, cache_error))
          status.cache_error = cache_error;
        cache_error.clear();
        if (!cache_->writeSnapshot(*current, cache_error) && status.cache_error.empty())
          status.cache_error = cache_error;
      }
    }
    const bool status_changed = initial || status.offline != offline_before ||
                                status.stale != stale_before;
    markDirtyPanels(source, previous.get(), *current, now_ms, status_changed);
    source.next_poll_ms = now_ms + sourceInterval(source);
    source.initial_fanout = false;
    ++tick.succeeded;
  }
  for (const auto& source : sources_) servicePanels(*source.second, now_ms);
  tick.dirty_panels = dirty_panels_.size();
  return tick;
}

void DataPipeline::failSource(SourceRuntime& source, const std::string& failure,
                              int64_t now_ms, TickResult& tick) {
  SourceStatus& status = source.status;
  const bool initial = source.initial_fanout;
  const bool offline_before = status.offline;
  const bool stale_before = status.stale;
  status.error = failure.empty() ? "source produced no result" : failure;
  status.offline = true;
  status.consecutive_failures = std::min<uint32_t>(status.consecutive_failures + 1, 1000000);
  if (!source.result) {
    status.stale = true;
  } else if (source.plan.stale_after_ms == 0) {
    status.stale = false;
  }
  if (initial || status.offline != offline_before || status.stale != stale_before)
    markStatusPanels(source, now_ms);
  const uint32_t shift = std::min<uint32_t>(status.consecutive_failures, 6);
  const uint64_t backoff = std::min<uint64_t>(
      static_cast<uint64_t>(sourceInterval(source)) << shift, 1800000ULL);
  source.next_poll_ms = now_ms + static_cast<int64_t>(backoff);
  source.initial_fanout = false;
  ++tick.failed;
}

void DataPipeline::succeedSource(SourceRuntime& source,
                                 std::shared_ptr<const SourceResult> current,
                                 bool not_modified, int64_t now_ms, TickResult& tick) {
  SourceStatus& status = source.status;
  const std::shared_ptr<const SourceResult> previous = source.result;
  const bool initial = source.initial_fanout;
  const bool offline_before = status.offline;
  const bool stale_before = status.stale;
  source.result = std::move(current);
  status.last_success_ms = now_ms;
  status.offline = false;
  status.stale = false;
  status.error.clear();
  status.cache_error.clear();
  status.consecutive_failures = 0;
  if (!not_modified) {
    std::string history_error;
    if (!history_.append(*source.result, now_ms, history_error))
      status.cache_error = "history: " + history_error;
    if (cache_ != nullptr) {
      std::vector<HistoryEntry> entries;
      entries.reserve(source.result->metrics.size());
      for (const MetricSample& sample : source.result->metrics) {
        HistoryEntry entry;
        entry.sample = sample;
        entry.reset_at_ms = source.result->resetAtFor(sample.metric);
        entries.push_back(std::move(entry));
      }
      std::string cache_error;
      if (!entries.empty() && !cache_->appendHistory(source.result->source_id, entries, cache_error))
        status.cache_error = cache_error;
      cache_error.clear();
      if (!cache_->writeSnapshot(*source.result, cache_error) && status.cache_error.empty())
        status.cache_error = cache_error;
    }
  }
  if (initial || status.offline != offline_before || status.stale != stale_before)
    markDirtyPanels(source, previous.get(), *source.result, now_ms, true);
  else
    markDirtyPanels(source, previous.get(), *source.result, now_ms, false);
  source.next_poll_ms = now_ms + sourceInterval(source);
  source.initial_fanout = false;
  ++tick.succeeded;
}

void DataPipeline::completeAsyncSource(SourceRuntime& source, const HttpResponse& response,
                                       int64_t now_ms, TickResult& tick) {
  source.status.last_http_status = response.status;
  const bool not_modified = response.not_modified || response.status == 304;
  std::shared_ptr<const SourceResult> current;
  std::string failure;
  if (not_modified) {
    if (!source.result) {
      failure = "304 response has no cached data";
    } else {
      auto copy = std::make_shared<SourceResult>(*source.result);
      copy->fetched_at_ms = now_ms;
      current = std::move(copy);
    }
  } else {
    SourceResult parsed;
    if (parseSourceResponse(source.plan, response, variables_, now_ms, parsed, failure))
      current = std::make_shared<const SourceResult>(std::move(parsed));
  }
  if (!failure.empty() || !current) {
    failSource(source, failure, now_ms, tick);
  } else {
    succeedSource(source, std::move(current), not_modified, now_ms, tick);
  }
}

void DataPipeline::detachAsync(SourceRuntime& source) {
  if (!source.async_pending) return;
  const uint64_t id = source.async_id;
  source.async_pending = false;
  source.async_id = 0;
  source.async_deadline_ms = 0;
  const auto batch = async_batches_.find(id);
  if (batch == async_batches_.end()) return;
  auto& ids = batch->second.source_ids;
  ids.erase(std::remove(ids.begin(), ids.end(), source.plan.source.id), ids.end());
  if (ids.empty()) {
    async_request_keys_.erase(batch->second.key);
    async_batches_.erase(batch);
  }
}

void DataPipeline::detachAllAsync() {
  for (auto& item : sources_) detachAsync(*item.second);
}

void DataPipeline::drainAsync(int64_t now_ms, TickResult& tick) {
  for (size_t count = 0; count < 32; ++count) {
    AsyncHttpResult result;
    if (!transport_.pollAsync(result)) break;
    const auto batch = async_batches_.find(result.id);
    if (batch == async_batches_.end()) continue;
    const AsyncBatch completed = batch->second;
    async_request_keys_.erase(completed.key);
    async_batches_.erase(batch);
    for (const std::string& source_id : completed.source_ids) {
      const auto found = sources_.find(source_id);
      if (found == sources_.end()) continue;
      SourceRuntime& source = *found->second;
      if (!source.async_pending || source.async_id != result.id) continue;
      source.async_pending = false;
      source.async_id = 0;
      source.async_deadline_ms = 0;
      completeAsyncSource(source, result.response, now_ms, tick);
    }
  }
  for (auto& item : sources_) {
    SourceRuntime& source = *item.second;
    if (!source.async_pending || now_ms <= source.async_deadline_ms) continue;
    detachAsync(source);
    HttpResponse timeout;
    timeout.status = -1;
    timeout.error = "asynchronous HTTP request timed out";
    completeAsyncSource(source, timeout, now_ms, tick);
  }
}

TickResult DataPipeline::tickAsync(int64_t now_ms) {
  TickResult tick;
  drainAsync(now_ms, tick);
  if (async_suspended_) {
    for (const auto& source : sources_) servicePanels(*source.second, now_ms);
    tick.dirty_panels = dirty_panels_.size();
    return tick;
  }
  for (auto& item : sources_) {
    SourceRuntime& source = *item.second;
    SourceStatus& status = source.status;
    const bool was_stale = status.stale;
    if (source.result) {
      status.stale = source.plan.stale_after_ms != 0 &&
                     (status.last_success_ms <= 0 ||
                      (now_ms >= status.last_success_ms &&
                       now_ms - status.last_success_ms > source.plan.stale_after_ms));
    } else {
      status.stale = true;
    }
    if (status.stale != was_stale) markStatusPanels(source, now_ms);
    if (!source.plan.source.enabled || source.async_pending || now_ms < source.next_poll_ms)
      continue;

    ++tick.due_sources;
    status.last_attempt_ms = now_ms;
    if (source.plan.source.kind == SourceKind::Static) {
      status.last_http_status = 200;
      std::shared_ptr<const SourceResult> current;
      std::string failure;
      if (source.static_result) {
        auto copy = std::make_shared<SourceResult>(*source.static_result);
        copy->fetched_at_ms = now_ms;
        current = std::move(copy);
      } else {
        HttpResponse response;
        response.status = 200;
        if (!interpolateVariables(source.plan.static_body, variables_, response.body, failure,
                                  source.plan.source.max_response_bytes)) {
          // failure already contains the interpolation error.
        } else {
          SourceResult parsed;
          if (parseSourceResponse(source.plan, response, variables_, now_ms, parsed, failure))
            current = std::make_shared<const SourceResult>(std::move(parsed));
        }
      }
      if (!failure.empty() || !current) {
        failSource(source, failure, now_ms, tick);
      } else {
        succeedSource(source, std::move(current), false, now_ms, tick);
      }
      continue;
    }

    net::NetRequest request;
    std::string failure;
    if (!buildSourceRequest(source.plan, variables_, config_.token_resolver, request, failure)) {
      status.last_http_status = 0;
      failSource(source, failure, now_ms, tick);
      continue;
    }
    const std::string key = sourceRequestKey(request);
    const int64_t deadline = now_ms + static_cast<int64_t>(source.plan.source.timeout_ms) + 5000;
    const auto existing = async_request_keys_.find(key);
    if (existing != async_request_keys_.end()) {
      AsyncBatch& batch = async_batches_.at(existing->second);
      if (std::find(batch.source_ids.begin(), batch.source_ids.end(), source.plan.source.id) ==
          batch.source_ids.end()) {
        batch.source_ids.push_back(source.plan.source.id);
      }
      source.async_pending = true;
      source.async_id = existing->second;
      source.async_deadline_ms = deadline;
      ++tick.deduplicated_requests;
      continue;
    }

    const uint64_t id = nextAsyncId();
    if (!transport_.executeAsync(id, request)) {
      failSource(source, "asynchronous HTTP queue unavailable", now_ms, tick);
      continue;
    }
    AsyncBatch batch;
    batch.id = id;
    batch.key = key;
    batch.source_ids.push_back(source.plan.source.id);
    async_batches_.emplace(id, std::move(batch));
    async_request_keys_[key] = id;
    source.async_pending = true;
    source.async_id = id;
    source.async_deadline_ms = deadline;
    ++tick.http_requests;
  }
  for (const auto& source : sources_) servicePanels(*source.second, now_ms);
  tick.dirty_panels = dirty_panels_.size();
  return tick;
}

void DataPipeline::cancelAsync() {
  detachAllAsync();
  async_batches_.clear();
  async_request_keys_.clear();
  transport_.cancelAsync();
  async_suspended_ = true;
}

void DataPipeline::resumeAsync() { async_suspended_ = false; }

bool DataPipeline::asyncIdle() const {
  return async_batches_.empty() && transport_.asyncIdle();
}

bool DataPipeline::refreshSource(const std::string& source_id) {
  const auto source = sources_.find(source_id);
  if (source == sources_.end() || !source->second->plan.source.enabled) return false;
  detachAsync(*source->second);
  source->second->next_poll_ms = 0;
  return true;
}

bool DataPipeline::refreshPanel(const std::string& panel_id) {
  const auto panel = panels_.find(panel_id);
  if (panel == panels_.end()) return false;
  panel->second.pending = true;
  panel->second.next_refresh_ms = 0;
  SourceRuntime& source = *sources_[panel->second.binding.source_id];
  detachAsync(source);
  source.next_poll_ms = 0;
  return true;
}

std::shared_ptr<const SourceResult> DataPipeline::sourceResult(const std::string& source_id) const {
  const auto source = sources_.find(source_id);
  return source == sources_.end() ? nullptr : source->second->result;
}

bool DataPipeline::sourceStatus(const std::string& source_id, SourceStatus& out) const {
  const auto source = sources_.find(source_id);
  if (source == sources_.end()) return false;
  out = source->second->status;
  return true;
}

int64_t DataPipeline::nextWakeMs(int64_t now_ms) const {
  int64_t next = now_ms + 1000;
  for (const auto& item : sources_) {
    const SourceRuntime& source = *item.second;
    if (source.plan.source.enabled && !source.async_pending)
      next = std::min(next, source.next_poll_ms);
    if (source.async_pending) next = std::min(next, source.async_deadline_ms);
    if (source.result && source.plan.stale_after_ms != 0) {
      next = std::min(next, source.status.last_success_ms + source.plan.stale_after_ms + 1);
    }
  }
  return std::max<int64_t>(next - now_ms, 10);
}

std::vector<std::string> DataPipeline::takeDirtyPanels() {
  std::vector<std::string> out(dirty_panels_.begin(), dirty_panels_.end());
  dirty_panels_.clear();
  return out;
}

}  // namespace eego::dashboard::pipeline
