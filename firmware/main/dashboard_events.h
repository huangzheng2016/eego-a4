// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "alert_core/types.h"
#include "dashboard_core/model.h"
#include "data_pipeline/types.h"

namespace panel {

inline constexpr char kDashboardAlertCorePrefix[] = "alert_core:";

inline eego::dashboard::Event dashboardEventFromAlert(const alert_core::AlertRecord& record,
                                                      const std::string& linked_panel_id) {
  eego::dashboard::Event event;
  event.id = std::string(kDashboardAlertCorePrefix) + record.id;
  event.rule_id = record.type;
  event.source_id = record.source.id;
  event.timestamp_ms = static_cast<int64_t>(record.last_seen) * 1000;
  event.title = record.title.empty() ? record.type : record.title;
  event.message = record.message;
  event.linked_panel_id = linked_panel_id;
  switch (record.severity) {
    case alert_core::Severity::Critical: event.severity = eego::dashboard::Severity::Critical; break;
    case alert_core::Severity::Warning: event.severity = eego::dashboard::Severity::Warning; break;
    default: event.severity = eego::dashboard::Severity::Info; break;
  }
  event.state = record.state == alert_core::AlertState::Active
                    ? eego::dashboard::EventState::Active
                    : eego::dashboard::EventState::Cleared;
  return event;
}

inline bool dashboardAlertRecordVisible(const alert_core::AlertRecord& record) {
  return !record.muted;
}

inline std::string dashboardCoreRecordId(const eego::dashboard::Event& event) {
  const std::string prefix(kDashboardAlertCorePrefix);
  return event.id.rfind(prefix, 0) == 0 ? event.id.substr(prefix.size()) : std::string();
}

inline std::string dashboardEventIdentity(const std::string& source_id,
                                          const std::string& event_id) {
  return source_id + "\n" + event_id;
}

inline std::string dashboardEventIdentity(const eego::dashboard::Event& event) {
  return dashboardEventIdentity(event.source_id, event.id);
}

inline std::set<std::string> dashboardAlertLinkUnion(const std::set<std::string>& previous,
                                                     const std::set<std::string>& current) {
  std::set<std::string> out = previous;
  out.insert(current.begin(), current.end());
  return out;
}

inline bool dashboardInjectionOutdated(
    const std::shared_ptr<const eego::dashboard::pipeline::SourceResult>& previous,
    const std::shared_ptr<const eego::dashboard::pipeline::SourceResult>& current) {
  return previous != current;
}

inline bool dashboardSourceEventsEqual(const eego::dashboard::pipeline::SourceResult* left,
                                       const eego::dashboard::pipeline::SourceResult* right) {
  const std::vector<eego::dashboard::Event> empty;
  const auto& a = left == nullptr ? empty : left->events;
  const auto& b = right == nullptr ? empty : right->events;
  if (a.size() != b.size()) return false;
  std::vector<const eego::dashboard::Event*> aa;
  std::vector<const eego::dashboard::Event*> bb;
  for (const auto& event : a) aa.push_back(&event);
  for (const auto& event : b) bb.push_back(&event);
  auto order = [](const eego::dashboard::Event* lhs, const eego::dashboard::Event* rhs) {
    if (lhs->id != rhs->id) return lhs->id < rhs->id;
    if (lhs->timestamp_ms != rhs->timestamp_ms) return lhs->timestamp_ms < rhs->timestamp_ms;
    return lhs->state < rhs->state;
  };
  std::sort(aa.begin(), aa.end(), order);
  std::sort(bb.begin(), bb.end(), order);
  for (size_t i = 0; i < aa.size(); ++i) {
    const auto& x = *aa[i];
    const auto& y = *bb[i];
    if (x.id != y.id || x.rule_id != y.rule_id || x.source_id != y.source_id ||
        x.timestamp_ms != y.timestamp_ms || x.severity != y.severity || x.state != y.state ||
        x.title != y.title || x.message != y.message || x.linked_panel_id != y.linked_panel_id) {
      return false;
    }
  }
  return true;
}

struct DashboardSourceChange {
  bool content_changed = false;
  bool status_changed = false;
  bool events_changed = false;
};

inline bool dashboardSourceStatusEqual(const eego::dashboard::pipeline::SourceStatus& left,
                                       const eego::dashboard::pipeline::SourceStatus& right) {
  return left.offline == right.offline && left.stale == right.stale &&
         left.consecutive_failures == right.consecutive_failures && left.error == right.error &&
         left.cache_error == right.cache_error;
}

inline bool dashboardMetricSamplesEqual(const eego::dashboard::MetricSample& left,
                                        const eego::dashboard::MetricSample& right) {
  auto labels = [](const eego::dashboard::MetricSample& sample) {
    std::vector<eego::dashboard::KeyValue> out = sample.labels;
    std::sort(out.begin(), out.end(), [](const eego::dashboard::KeyValue& a,
                                         const eego::dashboard::KeyValue& b) {
      return a.key == b.key ? a.value < b.value : a.key < b.key;
    });
    return out;
  };
  const auto a = labels(left);
  const auto b = labels(right);
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].key != b[i].key || a[i].value != b[i].value) return false;
  }
  return left.source_id == right.source_id && left.metric == right.metric &&
         left.timestamp_ms == right.timestamp_ms && left.value == right.value &&
         left.unit == right.unit;
}

inline bool dashboardSourceResultsEqual(const eego::dashboard::pipeline::SourceResult* left,
                                        const eego::dashboard::pipeline::SourceResult* right) {
  if (left == nullptr || right == nullptr) return left == right;
  if (left->source_id != right->source_id || left->reset_at_ms != right->reset_at_ms ||
      left->has_text != right->has_text || left->has_image != right->has_image ||
      left->metrics.size() != right->metrics.size() ||
      !dashboardSourceEventsEqual(left, right)) {
    return false;
  }
  if (left->has_text && left->text.text != right->text.text) return false;
  if (left->has_image && (left->image.media_type != right->image.media_type ||
                          left->image.bytes != right->image.bytes)) {
    return false;
  }
  std::vector<const eego::dashboard::MetricSample*> a;
  std::vector<const eego::dashboard::MetricSample*> b;
  for (const auto& sample : left->metrics) a.push_back(&sample);
  for (const auto& sample : right->metrics) b.push_back(&sample);
  auto order = [](const eego::dashboard::MetricSample* lhs,
                  const eego::dashboard::MetricSample* rhs) {
    if (lhs->source_id != rhs->source_id) return lhs->source_id < rhs->source_id;
    if (lhs->metric != rhs->metric) return lhs->metric < rhs->metric;
    return lhs->timestamp_ms < rhs->timestamp_ms;
  };
  std::sort(a.begin(), a.end(), order);
  std::sort(b.begin(), b.end(), order);
  for (size_t i = 0; i < a.size(); ++i) {
    if (!dashboardMetricSamplesEqual(*a[i], *b[i])) return false;
  }
  return true;
}

inline DashboardSourceChange dashboardSourceChangeOf(
    const eego::dashboard::pipeline::SourceResult* left,
    const eego::dashboard::pipeline::SourceResult* right,
    const eego::dashboard::pipeline::SourceStatus& left_status,
    const eego::dashboard::pipeline::SourceStatus& right_status) {
  DashboardSourceChange change;
  change.events_changed = !dashboardSourceEventsEqual(left, right);
  change.content_changed = !dashboardSourceResultsEqual(left, right);
  change.status_changed = !dashboardSourceStatusEqual(left_status, right_status);
  return change;
}

inline bool dashboardShouldDisplay(bool renderer_refresh_needed, bool status_dirty) {
  return renderer_refresh_needed || status_dirty;
}

class DashboardEventTracker {
 public:
  uint64_t observe(const eego::dashboard::Event& event) {
    Generation& current = generations_[dashboardEventIdentity(event)];
    if (current.generation == 0) {
      current.generation = 1;
      current.last_state = event.state;
      current.last_timestamp_ms = event.timestamp_ms;
      current.last_severity = event.severity;
      return current.generation;
    }
    if (event.timestamp_ms < current.last_timestamp_ms) return current.generation;
    const bool reactivation = event.state == eego::dashboard::EventState::Active &&
                              current.last_state != eego::dashboard::EventState::Active;
    const bool escalation = event.state == eego::dashboard::EventState::Active &&
                            current.last_state == eego::dashboard::EventState::Active &&
                            static_cast<uint8_t>(event.severity) >
                                static_cast<uint8_t>(current.last_severity);
    if (reactivation || escalation) ++current.generation;
    current.last_state = event.state;
    current.last_timestamp_ms = event.timestamp_ms;
    current.last_severity = event.severity;
    return current.generation;
  }

  uint64_t generation(const std::string& identity) const {
    const auto found = generations_.find(identity);
    return found == generations_.end() ? 0 : found->second.generation;
  }

  uint64_t generation(const eego::dashboard::Event& event) const {
    return generation(dashboardEventIdentity(event));
  }

  void acknowledge(const std::string& identity, uint64_t generation) {
    if (generation != 0) acknowledged_[identity] = generation;
  }

  void acknowledge(const eego::dashboard::Event& event, uint64_t generation) {
    acknowledge(dashboardEventIdentity(event), generation);
  }

  void eraseAcknowledgement(const std::string& identity) { acknowledged_.erase(identity); }

  void eraseAcknowledgement(const eego::dashboard::Event& event) {
    eraseAcknowledgement(dashboardEventIdentity(event));
  }

  void eraseAcknowledgement(const std::string& source_id, const std::string& event_id) {
    eraseAcknowledgement(dashboardEventIdentity(source_id, event_id));
  }

  bool suppressed(const eego::dashboard::Event& event, uint64_t generation) const {
    const auto found = acknowledged_.find(dashboardEventIdentity(event));
    return event.state == eego::dashboard::EventState::Active && found != acknowledged_.end() &&
           found->second == generation;
  }

  void reconcile(const std::vector<eego::dashboard::Event>& current_events) {
    std::set<std::string> live;
    for (const eego::dashboard::Event& event : current_events)
      live.insert(dashboardEventIdentity(event));
    for (auto it = generations_.begin(); it != generations_.end();) {
      if (live.count(it->first) == 0) {
        it = generations_.erase(it);
      } else {
        ++it;
      }
    }
    for (auto it = acknowledged_.begin(); it != acknowledged_.end();) {
      if (live.count(it->first) == 0) {
        it = acknowledged_.erase(it);
      } else {
        ++it;
      }
    }
  }

  size_t generationCount() const { return generations_.size(); }
  size_t acknowledgementCount() const { return acknowledged_.size(); }

  void reset() {
    generations_.clear();
    acknowledged_.clear();
  }

 private:
  struct Generation {
    uint64_t generation = 0;
    eego::dashboard::EventState last_state = eego::dashboard::EventState::Cleared;
    eego::dashboard::Severity last_severity = eego::dashboard::Severity::Info;
    int64_t last_timestamp_ms = -1;
  };

  std::map<std::string, Generation> generations_;
  std::map<std::string, uint64_t> acknowledged_;
};

inline const eego::dashboard::AlertRule* dashboardRuleForEvent(
    const std::vector<eego::dashboard::AlertRule>& rules, const eego::dashboard::Event& event) {
  for (const eego::dashboard::AlertRule& rule : rules) {
    if (rule.id == event.rule_id) return &rule;
  }
  return nullptr;
}

inline const eego::dashboard::Event* dashboardBannerEvent(
    const std::vector<eego::dashboard::Event>& events,
    const std::vector<eego::dashboard::AlertRule>& rules,
    const DashboardEventTracker& tracker) {
  const eego::dashboard::Event* selected = nullptr;
  for (const eego::dashboard::Event& event : events) {
    if (event.state != eego::dashboard::EventState::Active ||
        tracker.suppressed(event, tracker.generation(event))) {
      continue;
    }
    const eego::dashboard::AlertRule* rule = dashboardRuleForEvent(rules, event);
    if (rule == nullptr || (rule->style.surfaces & eego::dashboard::kAlertSurfaceBanner) == 0)
      continue;
    if (selected == nullptr || event.timestamp_ms > selected->timestamp_ms ||
        (event.timestamp_ms == selected->timestamp_ms &&
         static_cast<uint8_t>(event.severity) > static_cast<uint8_t>(selected->severity))) {
      selected = &event;
    }
  }
  return selected;
}

inline std::string dashboardBannerSignature(
    const std::vector<eego::dashboard::Event>& events,
    const std::vector<eego::dashboard::AlertRule>& rules,
    const DashboardEventTracker& tracker) {
  const eego::dashboard::Event* event = dashboardBannerEvent(events, rules, tracker);
  if (event == nullptr) return {};
  return dashboardEventIdentity(*event) + "\n" + eego::dashboard::severityName(event->severity) +
         "\n" + event->title;
}

}  // namespace panel
