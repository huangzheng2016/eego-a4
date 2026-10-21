// SPDX-License-Identifier: GPL-3.0-or-later
#include "data_pipeline/history.h"

#include "dashboard_core/json.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace eego::dashboard::pipeline {
namespace {

constexpr int64_t kHourMs = 60LL * 60 * 1000;
constexpr int64_t kDayMs = 24 * kHourMs;

int64_t floorUnit(int64_t value, int64_t unit) {
  int64_t quotient = value / unit;
  if (value < 0 && value % unit != 0) --quotient;
  return quotient;
}

int64_t floorDay(int64_t value) { return floorUnit(value, kDayMs); }

int64_t windowEnd(WindowKind kind, int64_t from_ms) {
  const int64_t duration = kind == WindowKind::Fixed5h ? 5 * kHourMs
                           : kind == WindowKind::Fixed7d ? 7 * kDayMs
                                                         : kDayMs;
  return from_ms > std::numeric_limits<int64_t>::max() - duration
             ? std::numeric_limits<int64_t>::max()
             : from_ms + duration;
}

void appendKey(std::string& key, std::string_view value) {
  key += std::to_string(value.size());
  key.push_back(':');
  key.append(value);
  key.push_back(';');
}

std::vector<KeyValue> canonicalLabels(std::vector<KeyValue> labels) {
  std::sort(labels.begin(), labels.end(), [](const KeyValue& left, const KeyValue& right) {
    return left.key == right.key ? left.value < right.value : left.key < right.key;
  });
  return labels;
}

bool labelsMatch(const std::vector<KeyValue>& labels, const std::vector<KeyValue>& selector) {
  for (const KeyValue& wanted : selector) {
    const auto it = std::find_if(labels.begin(), labels.end(), [&](const KeyValue& label) {
      return label.key == wanted.key;
    });
    if (it == labels.end() || it->value != wanted.value) return false;
  }
  return true;
}

struct BucketStats {
  size_t count = 1;
  double sum = 0.0;
  double min = 0.0;
  double max = 0.0;
  double first_value = 0.0;
  double last_value = 0.0;
  int64_t first_timestamp_ms = 0;
};

BucketStats statsFor(const HistoryEntry& entry) {
  if (entry.count > 0) {
    return {entry.count, entry.sum, entry.min, entry.max, entry.first_value,
            entry.last_value, entry.first_timestamp_ms};
  }
  const double value = entry.sample.value;
  return {1, value, value, value, value, value, entry.sample.timestamp_ms};
}

void initializeStats(HistoryEntry& entry) {
  if (entry.count != 0) return;
  const BucketStats stats = statsFor(entry);
  entry.count = stats.count;
  entry.sum = stats.sum;
  entry.min = stats.min;
  entry.max = stats.max;
  entry.first_value = stats.first_value;
  entry.last_value = stats.last_value;
  entry.first_timestamp_ms = stats.first_timestamp_ms;
}

bool validSample(const MetricSample& sample) {
  return !sample.source_id.empty() && !sample.metric.empty() && jsonIsValidUtf8(sample.metric) &&
         jsonCodePointCount(sample.metric) <= 128 && jsonIsValidUtf8(sample.unit) &&
         jsonCodePointCount(sample.unit) <= 32 && sample.timestamp_ms >= 0 &&
         std::isfinite(sample.value) && sample.labels.size() <= kMaxLabels;
}

HistoryLimits sanitize(HistoryLimits limits) {
  limits.max_series = std::max<size_t>(limits.max_series, 1);
  limits.max_points_per_series = std::max<size_t>(limits.max_points_per_series, 2);
  if (limits.downsample_points == 0 ||
      limits.downsample_points >= limits.max_points_per_series) {
    limits.downsample_points = limits.max_points_per_series / 2;
  }
  limits.max_raw_points_per_series = std::max<size_t>(limits.max_raw_points_per_series, 1);
  limits.max_total_points = std::max<size_t>(limits.max_total_points, 1);
  limits.retention_ms = std::max<int64_t>(limits.retention_ms, 60000);
  return limits;
}

}  // namespace

ResolvedWindow resolveWindow(WindowKind kind, int64_t now_ms, int64_t reset_at_ms,
                             int32_t timezone_offset_minutes) {
  now_ms = std::max<int64_t>(now_ms, 0);
  int64_t duration = kDayMs;
  if (kind == WindowKind::Fixed5h) duration = 5 * kHourMs;
  if (kind == WindowKind::Fixed7d) duration = 7 * kDayMs;
  if (kind == WindowKind::Today) {
    const int32_t bounded_offset =
        std::max<int32_t>(-1440, std::min<int32_t>(1440, timezone_offset_minutes));
    const int64_t offset = static_cast<int64_t>(bounded_offset) * 60000;
    const int64_t shifted = offset > 0 && now_ms > std::numeric_limits<int64_t>::max() - offset
                                ? std::numeric_limits<int64_t>::max()
                                : now_ms + offset;
    const int64_t local_day = floorDay(shifted) * kDayMs;
    const int64_t from = offset < 0 && local_day > std::numeric_limits<int64_t>::max() + offset
                             ? std::numeric_limits<int64_t>::max()
                             : local_day - offset;
    return {std::max<int64_t>(from, 0), now_ms};
  }
  if (kind == WindowKind::Fixed5h || kind == WindowKind::Fixed7d) {
    if (reset_at_ms > 0)
      return {std::max<int64_t>(reset_at_ms - duration, 0), reset_at_ms};
    const int32_t bounded_offset =
        std::max<int32_t>(-1440, std::min<int32_t>(1440, timezone_offset_minutes));
    const int64_t offset = static_cast<int64_t>(bounded_offset) * 60000;
    const int64_t shifted = offset > 0 && now_ms > std::numeric_limits<int64_t>::max() - offset
                                ? std::numeric_limits<int64_t>::max()
                                : now_ms + offset;
    const int64_t local_start = floorUnit(shifted, duration) * duration;
    const int64_t from = offset < 0 && local_start > std::numeric_limits<int64_t>::max() + offset
                             ? std::numeric_limits<int64_t>::max()
                             : local_start - offset;
    return {std::max<int64_t>(from, 0), now_ms};
  }
  return {std::max<int64_t>(now_ms - duration, 0), now_ms};
}

AggregationResult aggregateSamples(const std::vector<MetricSample>& samples,
                                   Aggregation aggregation) {
  std::vector<const MetricSample*> ordered;
  ordered.reserve(samples.size());
  for (const MetricSample& sample : samples) {
    if (sample.timestamp_ms >= 0 && std::isfinite(sample.value)) ordered.push_back(&sample);
  }
  std::stable_sort(ordered.begin(), ordered.end(), [](const MetricSample* left,
                                                      const MetricSample* right) {
    return left->timestamp_ms < right->timestamp_ms;
  });
  AggregationResult result;
  result.sample_count = ordered.size();
  if (ordered.empty()) return result;
  result.first_timestamp_ms = ordered.front()->timestamp_ms;
  result.last_timestamp_ms = ordered.back()->timestamp_ms;
  double value = ordered.front()->value;
  if (aggregation == Aggregation::Sum || aggregation == Aggregation::Avg) value = 0.0;
  for (const MetricSample* sample : ordered) {
    if (aggregation == Aggregation::Sum || aggregation == Aggregation::Avg)
      value += sample->value;
    else if (aggregation == Aggregation::Min)
      value = std::min(value, sample->value);
    else if (aggregation == Aggregation::Max)
      value = std::max(value, sample->value);
  }
  if (aggregation == Aggregation::Avg) value /= static_cast<double>(ordered.size());
  if (aggregation == Aggregation::Delta || aggregation == Aggregation::Rate)
    value = ordered.back()->value - ordered.front()->value;
  if (aggregation == Aggregation::Rate) {
    const int64_t elapsed = result.last_timestamp_ms - result.first_timestamp_ms;
    if (elapsed <= 0) return result;
    value /= static_cast<double>(elapsed) / 1000.0;
  }
  if (!std::isfinite(value)) return result;
  result.value = value;
  result.valid = true;
  return result;
}

AggregationResult aggregateHistoryEntries(const std::vector<HistoryEntry>& entries,
                                          Aggregation aggregation) {
  std::vector<const HistoryEntry*> ordered;
  ordered.reserve(entries.size());
  for (const HistoryEntry& entry : entries) {
    if (entry.sample.timestamp_ms >= 0 && std::isfinite(entry.sample.value))
      ordered.push_back(&entry);
  }
  std::stable_sort(ordered.begin(), ordered.end(), [](const HistoryEntry* left,
                                                      const HistoryEntry* right) {
    return left->sample.timestamp_ms < right->sample.timestamp_ms;
  });
  AggregationResult result;
  if (ordered.empty()) return result;
  for (const HistoryEntry* entry : ordered) result.sample_count += statsFor(*entry).count;
  const BucketStats first = statsFor(*ordered.front());
  const BucketStats last = statsFor(*ordered.back());
  result.first_timestamp_ms = first.first_timestamp_ms;
  result.last_timestamp_ms = ordered.back()->sample.timestamp_ms;
  double value = aggregation == Aggregation::Min ? first.min
                 : aggregation == Aggregation::Max ? first.max
                                                   : 0.0;
  for (const HistoryEntry* entry : ordered) {
    const BucketStats stats = statsFor(*entry);
    if (aggregation == Aggregation::Sum || aggregation == Aggregation::Avg)
      value += stats.sum;
    else if (aggregation == Aggregation::Min)
      value = std::min(value, stats.min);
    else if (aggregation == Aggregation::Max)
      value = std::max(value, stats.max);
  }
  if (aggregation == Aggregation::Avg) value /= static_cast<double>(result.sample_count);
  if (aggregation == Aggregation::Delta || aggregation == Aggregation::Rate)
    value = last.last_value - first.first_value;
  if (aggregation == Aggregation::Rate) {
    const int64_t elapsed = result.last_timestamp_ms - result.first_timestamp_ms;
    if (elapsed <= 0) return result;
    value /= static_cast<double>(elapsed) / 1000.0;
  }
  if (!std::isfinite(value)) return result;
  result.value = value;
  result.valid = true;
  return result;
}

void downsampleEntries(std::vector<HistoryEntry>& entries, size_t target_points) {
  if (target_points == 0 || entries.size() <= target_points) return;
  for (HistoryEntry& entry : entries) initializeStats(entry);
  std::stable_sort(entries.begin(), entries.end(), [](const HistoryEntry& left,
                                                      const HistoryEntry& right) {
    return left.sample.timestamp_ms < right.sample.timestamp_ms;
  });
  const size_t input_size = entries.size();
  std::vector<HistoryEntry> reduced;
  reduced.reserve(target_points);
  for (size_t bucket = 0; bucket < target_points; ++bucket) {
    const size_t begin = bucket * input_size / target_points;
    const size_t end = (bucket + 1) * input_size / target_points;
    if (begin >= end) continue;
    HistoryEntry combined = entries[end - 1];
    const BucketStats first = statsFor(entries[begin]);
    const BucketStats last = statsFor(entries[end - 1]);
    combined.count = 0;
    combined.sum = 0.0;
    combined.min = first.min;
    combined.max = first.max;
    combined.first_value = first.first_value;
    combined.last_value = last.last_value;
    combined.first_timestamp_ms = first.first_timestamp_ms;
    for (size_t i = begin; i < end; ++i) {
      const BucketStats stats = statsFor(entries[i]);
      combined.count += stats.count;
      combined.sum += stats.sum;
      combined.min = std::min(combined.min, stats.min);
      combined.max = std::max(combined.max, stats.max);
    }
    combined.sample.value = combined.sum / static_cast<double>(combined.count);
    combined.aggregated = true;
    reduced.push_back(std::move(combined));
  }
  entries = std::move(reduced);
}

AggregationResult incompleteAggregate() {
  AggregationResult result;
  result.complete = false;
  return result;
}

MetricHistory::MetricHistory(HistoryLimits limits, int32_t timezone_offset_minutes)
    : limits_(sanitize(limits)), timezone_offset_minutes_(timezone_offset_minutes) {}

void MetricHistory::recordLoss(WindowAccumulator& state, int64_t from_ms, int64_t to_ms) {
  if (from_ms < 0 || to_ms < 0) return;
  if (from_ms > to_ms) std::swap(from_ms, to_ms);
  state.coverage_lost = true;
  state.loss_from_ms = state.loss_from_ms < 0 ? from_ms : std::min(state.loss_from_ms, from_ms);
  state.loss_to_ms = state.loss_to_ms < 0 ? to_ms : std::max(state.loss_to_ms, to_ms);
}

void MetricHistory::propagateCoverageLoss(Series& series, const WindowKey& key) {
  const auto has_loss = [](const WindowAccumulator& state) {
    return state.coverage_lost && state.loss_from_ms >= 0 &&
           state.loss_to_ms >= state.loss_from_ms;
  };
  WindowAccumulator& state = series.windows.at(key);
  for (const auto& candidate : series.windows) {
    if (&candidate.second == &state || !has_loss(candidate.second)) continue;
    if (candidate.second.loss_from_ms <= windowEnd(key.kind, key.from_ms) &&
        candidate.second.loss_to_ms >= key.from_ms) {
      recordLoss(state, candidate.second.loss_from_ms, candidate.second.loss_to_ms);
    }
  }
  if (!has_loss(state)) return;
  for (auto& candidate : series.windows) {
    if (&candidate.second == &state) continue;
    if (state.loss_from_ms <= windowEnd(candidate.first.kind, candidate.first.from_ms) &&
        state.loss_to_ms >= candidate.first.from_ms) {
      recordLoss(candidate.second, state.loss_from_ms, state.loss_to_ms);
    }
  }
}

void MetricHistory::addWindowSample(Series& series, WindowKind kind, int64_t from_ms,
                                    const MetricSample& sample, bool coverage_lost,
                                    int64_t loss_from_ms, int64_t loss_to_ms) {
  const WindowKey key{kind, from_ms};
  auto it = series.windows.find(key);
  if (it == series.windows.end()) {
    const size_t limit = kind == WindowKind::Today ? 8 : kind == WindowKind::Fixed5h ? 40 : 4;
    size_t count = 0;
    auto oldest = series.windows.end();
    for (auto candidate = series.windows.begin(); candidate != series.windows.end(); ++candidate) {
      if (candidate->first.kind != kind) continue;
      ++count;
      if (oldest == series.windows.end() ||
          candidate->second.last_timestamp_ms < oldest->second.last_timestamp_ms) {
        oldest = candidate;
      }
    }
    if (count >= limit && oldest != series.windows.end()) {
      series.fixed_gap = true;
      series.windows.erase(oldest);
    }
    it = series.windows.emplace(key, WindowAccumulator{}).first;
  }
  WindowAccumulator& state = it->second;
  if (coverage_lost) {
    recordLoss(state, loss_from_ms >= 0 ? loss_from_ms : sample.timestamp_ms,
               loss_to_ms >= 0 ? loss_to_ms : sample.timestamp_ms);
  }
  propagateCoverageLoss(series, key);
  auto position = std::lower_bound(state.samples.begin(), state.samples.end(), sample.timestamp_ms,
                                   [](const std::pair<int64_t, double>& point, int64_t timestamp) {
                                     return point.first < timestamp;
                                   });
  if (position != state.samples.end() && position->first == sample.timestamp_ms) {
    if (position->second == sample.value) return;
    const double old_value = position->second;
    position->second = sample.value;
    state.sum += sample.value - old_value;
    if (old_value == state.min || old_value == state.max) {
      state.min = state.max = state.samples.front().second;
      for (const auto& point : state.samples) {
        state.min = std::min(state.min, point.second);
        state.max = std::max(state.max, point.second);
      }
    }
    if (sample.timestamp_ms == state.first_timestamp_ms) state.first_value = sample.value;
    if (sample.timestamp_ms == state.last_timestamp_ms) state.last_value = sample.value;
    if (!std::isfinite(state.sum)) recordLoss(state, sample.timestamp_ms, sample.timestamp_ms);
    if (state.coverage_lost) propagateCoverageLoss(series, key);
    return;
  }
  // Inclusive endpoints at the fastest supported 1s cadence; faster data is explicitly incomplete.
  const size_t limit = kind == WindowKind::Today
                           ? 86401
                           : kind == WindowKind::Fixed5h ? 18001 : 604801;
  if (state.samples.size() >= limit) {
    recordLoss(state, sample.timestamp_ms, sample.timestamp_ms);
    propagateCoverageLoss(series, key);
    return;
  }
  state.samples.insert(position, {sample.timestamp_ms, sample.value});
  state.count = state.samples.size();
  state.sum += sample.value;
  if (state.samples.size() == 1) {
    state.min = sample.value;
    state.max = sample.value;
    state.first_value = sample.value;
    state.last_value = sample.value;
    state.first_timestamp_ms = sample.timestamp_ms;
    state.last_timestamp_ms = sample.timestamp_ms;
  } else {
    state.min = std::min(state.min, sample.value);
    state.max = std::max(state.max, sample.value);
    if (sample.timestamp_ms < state.first_timestamp_ms) {
      state.first_timestamp_ms = sample.timestamp_ms;
      state.first_value = sample.value;
    }
    if (sample.timestamp_ms >= state.last_timestamp_ms) {
      state.last_timestamp_ms = sample.timestamp_ms;
      state.last_value = sample.value;
    }
  }
  if (!std::isfinite(state.sum)) recordLoss(state, sample.timestamp_ms, sample.timestamp_ms);
  if (state.coverage_lost) propagateCoverageLoss(series, key);
}

void MetricHistory::addFixed(Series& series, const MetricSample& sample) {
  const auto add = [&](WindowKind kind) {
    const int64_t duration = kind == WindowKind::Fixed5h ? 5 * kHourMs
                             : kind == WindowKind::Fixed7d ? 7 * kDayMs
                                                           : kDayMs;
    const int64_t from = kind != WindowKind::Today && series.reset_at_ms > 0
                             ? series.reset_at_ms - duration
                             : resolveWindow(kind, sample.timestamp_ms, 0,
                                             timezone_offset_minutes_)
                                   .from_ms;
    if (kind != WindowKind::Today && series.reset_at_ms > 0 &&
        (sample.timestamp_ms < from || sample.timestamp_ms > series.reset_at_ms)) {
      return;
    }
    addWindowSample(series, kind, from, sample);
  };
  add(WindowKind::Today);
  add(WindowKind::Fixed5h);
  add(WindowKind::Fixed7d);
}

void MetricHistory::pruneFixed(Series& series, int64_t cutoff_ms) {
  for (auto it = series.windows.begin(); it != series.windows.end();) {
    if (it->second.last_timestamp_ms < cutoff_ms) {
      series.fixed_gap = true;
      it = series.windows.erase(it);
    } else {
      ++it;
    }
  }
}

AggregationResult MetricHistory::aggregateFixed(const Series& series, WindowKind kind,
                                                const ResolvedWindow& window,
                                                Aggregation aggregation) const {
  const auto state = series.windows.find({kind, window.from_ms});
  if (state == series.windows.end()) {
    return series.fixed_gap ? incompleteAggregate() : AggregationResult{};
  }
  const WindowAccumulator& aggregate = state->second;
  if (aggregate.coverage_lost) return incompleteAggregate();
  AggregationResult result;
  result.sample_count = aggregate.count;
  result.first_timestamp_ms = aggregate.first_timestamp_ms;
  result.last_timestamp_ms = aggregate.last_timestamp_ms;
  if (aggregate.count == 0) return result;
  if (aggregation == Aggregation::Sum || aggregation == Aggregation::Avg) {
    result.value = aggregate.sum;
    if (aggregation == Aggregation::Avg)
      result.value /= static_cast<double>(aggregate.count);
  } else if (aggregation == Aggregation::Min) {
    result.value = aggregate.min;
  } else if (aggregation == Aggregation::Max) {
    result.value = aggregate.max;
  } else {
    result.value = aggregate.last_value - aggregate.first_value;
    if (aggregation == Aggregation::Rate) {
      const int64_t elapsed = aggregate.last_timestamp_ms - aggregate.first_timestamp_ms;
      if (elapsed <= 0) return result;
      result.value /= static_cast<double>(elapsed) / 1000.0;
    }
  }
  if (!std::isfinite(result.value)) return result;
  result.valid = true;
  return result;
}

std::string MetricHistory::keyFor(const MetricSample& sample) {
  std::string key;
  appendKey(key, sample.source_id);
  appendKey(key, sample.metric);
  for (const KeyValue& label : sample.labels) {
    appendKey(key, label.key);
    appendKey(key, label.value);
  }
  return key;
}

bool MetricHistory::appendEntry(HistoryEntry entry, std::string& error, bool add_fixed) {
  error.clear();
  if (!validSample(entry.sample)) {
    error = "history rejected an invalid metric sample";
    return false;
  }
  entry.sample.labels = canonicalLabels(std::move(entry.sample.labels));
  const std::string key = keyFor(entry.sample);
  auto it = series_.find(key);
  if (it == series_.end()) {
    if (series_.size() >= limits_.max_series) {
      error = "history series limit exceeded";
      return false;
    }
    Series series;
    series.source_id = entry.sample.source_id;
    series.metric = entry.sample.metric;
    series.labels = entry.sample.labels;
    it = series_.emplace(key, std::move(series)).first;
  }
  Series& series = it->second;
  series.reset_at_ms = std::max(series.reset_at_ms, entry.reset_at_ms);
  if (series.unit.empty()) series.unit = entry.sample.unit;
  auto position = std::lower_bound(series.raw_points.begin(), series.raw_points.end(),
                                   entry.sample.timestamp_ms,
                                   [](const MetricSample& point, int64_t timestamp) {
                                     return point.timestamp_ms < timestamp;
                                   });
  if (position != series.raw_points.end() &&
      position->timestamp_ms == entry.sample.timestamp_ms) {
    *position = std::move(entry.sample);
  } else {
    position = series.raw_points.insert(position, std::move(entry.sample));
  }
  if (add_fixed) addFixed(series, *position);
  HistoryEntry display;
  display.sample = *position;
  display.reset_at_ms = series.reset_at_ms;
  auto display_position = std::lower_bound(series.points.begin(), series.points.end(),
                                           display.sample.timestamp_ms,
                                           [](const HistoryEntry& point, int64_t timestamp) {
                                             return point.sample.timestamp_ms < timestamp;
                                           });
  if (display_position != series.points.end() &&
      display_position->sample.timestamp_ms == display.sample.timestamp_ms) {
    if (!display_position->aggregated) *display_position = std::move(display);
  } else {
    series.points.insert(display_position, std::move(display));
  }
  downsample(series);
  return true;
}

void MetricHistory::downsample(Series& series) {
  if (series.points.size() <= limits_.max_points_per_series) return;
  std::vector<HistoryEntry> points;
  points.reserve(series.points.size());
  for (const HistoryEntry& entry : series.points) points.push_back(entry);
  downsampleEntries(points, limits_.downsample_points);
  series.points.clear();
  for (HistoryEntry& point : points) series.points.push_back(std::move(point));
}

void MetricHistory::recomputeCounts() {
  point_count_ = 0;
  raw_point_count_ = 0;
  for (const auto& item : series_) {
    point_count_ += item.second.points.size();
    raw_point_count_ += item.second.raw_points.size();
  }
}

void MetricHistory::enforceLimits() {
  for (auto& item : series_) {
    Series& series = item.second;
    while (series.raw_points.size() > limits_.max_raw_points_per_series) {
      series.raw_gap_before_ms =
          std::max(series.raw_gap_before_ms, series.raw_points.front().timestamp_ms);
      series.raw_points.pop_front();
    }
  }
  recomputeCounts();
  while (raw_point_count_ > limits_.max_total_points && !series_.empty()) {
    auto oldest = series_.end();
    for (auto it = series_.begin(); it != series_.end(); ++it) {
      if (it->second.raw_points.empty()) continue;
      if (oldest == series_.end() ||
          it->second.raw_points.front().timestamp_ms <
              oldest->second.raw_points.front().timestamp_ms) {
        oldest = it;
      }
    }
    if (oldest == series_.end()) break;
    oldest->second.raw_gap_before_ms = std::max(
        oldest->second.raw_gap_before_ms, oldest->second.raw_points.front().timestamp_ms);
    oldest->second.raw_points.pop_front();
    recomputeCounts();
  }
}

bool MetricHistory::append(const SourceResult& result, int64_t now_ms, std::string& error) {
  error.clear();
  for (MetricSample sample : result.metrics) {
    if (sample.source_id.empty()) sample.source_id = result.source_id;
    if (sample.source_id != result.source_id) {
      error = "history rejected a cross-source metric sample";
      return false;
    }
    HistoryEntry entry;
    entry.sample = std::move(sample);
    entry.reset_at_ms = result.resetAtFor(entry.sample.metric);
    if (!appendEntry(std::move(entry), error)) return false;
  }
  prune(now_ms);
  enforceLimits();
  return true;
}

bool MetricHistory::replaceSource(const std::string& source_id,
                                  const std::vector<HistoryEntry>& entries, int64_t now_ms,
                                  std::string& error) {
  clearSource(source_id);
  std::vector<HistoryEntry> ordered = entries;
  std::stable_sort(ordered.begin(), ordered.end(), [](const HistoryEntry& left,
                                                      const HistoryEntry& right) {
    return left.sample.timestamp_ms < right.sample.timestamp_ms;
  });
  std::set<std::string> windowed_series;
  for (const HistoryEntry& entry : ordered) {
    if (!entry.window_state) continue;
    MetricSample canonical = entry.sample;
    canonical.labels = canonicalLabels(std::move(canonical.labels));
    windowed_series.insert(keyFor(canonical));
  }
  for (HistoryEntry& entry : ordered) {
    if (entry.window_state) continue;
    if (entry.sample.source_id.empty()) entry.sample.source_id = source_id;
    if (entry.sample.source_id != source_id) {
      error = "history rejected a cross-source cache entry";
      clearSource(source_id);
      return false;
    }
    MetricSample canonical = entry.sample;
    canonical.labels = canonicalLabels(std::move(canonical.labels));
    const std::string key = keyFor(canonical);
    const int64_t raw_gap = entry.raw_gap_before_ms;
    const bool fixed_gap = entry.fixed_gap;
    if (!appendEntry(std::move(entry), error, windowed_series.count(key) == 0)) {
      clearSource(source_id);
      return false;
    }
    Series& series = series_.at(key);
    series.raw_gap_before_ms = std::max(series.raw_gap_before_ms, raw_gap);
    series.fixed_gap = series.fixed_gap || fixed_gap;
  }
  for (const HistoryEntry& entry : ordered) {
    if (!entry.window_state) continue;
    if (entry.sample.source_id != source_id || entry.window_kind == WindowKind::Rolling24h ||
        entry.window_from_ms < 0 ||
        ((entry.loss_from_ms < 0) != (entry.loss_to_ms < 0)) ||
        (entry.loss_from_ms >= 0 &&
         (entry.loss_to_ms < entry.loss_from_ms || !entry.coverage_lost)) ||
        (!entry.coverage_sample && entry.count == 0 && !entry.coverage_lost) ||
        !std::isfinite(entry.sum) || !std::isfinite(entry.min) || !std::isfinite(entry.max) ||
        !std::isfinite(entry.first_value) || !std::isfinite(entry.last_value) ||
        entry.first_timestamp_ms < 0 ||
        entry.first_timestamp_ms > entry.sample.timestamp_ms) {
      error = "history rejected an invalid window cache entry";
      clearSource(source_id);
      return false;
    }
    MetricSample canonical = entry.sample;
    canonical.labels = canonicalLabels(std::move(canonical.labels));
    const std::string key = keyFor(canonical);
    auto it = series_.find(key);
    if (it == series_.end()) {
      if (series_.size() >= limits_.max_series) {
        error = "history series limit exceeded";
        clearSource(source_id);
        return false;
      }
      Series series;
      series.source_id = canonical.source_id;
      series.metric = canonical.metric;
      series.labels = canonical.labels;
      it = series_.emplace(key, std::move(series)).first;
    }
    Series& series = it->second;
    series.reset_at_ms = std::max(series.reset_at_ms, entry.reset_at_ms);
    series.raw_gap_before_ms = std::max(series.raw_gap_before_ms, entry.raw_gap_before_ms);
    if (series.unit.empty()) series.unit = canonical.unit;
    if (entry.coverage_sample) {
      if (entry.coverage_windows.empty()) {
        addWindowSample(series, entry.window_kind, entry.window_from_ms, canonical,
                        entry.coverage_lost, entry.loss_from_ms, entry.loss_to_ms);
      } else {
        for (const auto& window : entry.coverage_windows)
          addWindowSample(series, window.first, window.second, canonical, entry.coverage_lost,
                          entry.loss_from_ms, entry.loss_to_ms);
      }
      series.fixed_gap = series.fixed_gap || entry.fixed_gap;
      continue;
    }
    const WindowKey window_key{entry.window_kind, entry.window_from_ms};
    if (series.windows.find(window_key) == series.windows.end()) {
      WindowAccumulator state;
      state.count = entry.count;
      state.sum = entry.sum;
      state.min = entry.min;
      state.max = entry.max;
      state.first_value = entry.first_value;
      state.last_value = entry.last_value;
      state.first_timestamp_ms = entry.first_timestamp_ms;
      state.last_timestamp_ms = entry.sample.timestamp_ms;
      state.coverage_lost = entry.coverage_lost;
      state.loss_from_ms = entry.loss_from_ms;
      state.loss_to_ms = entry.loss_to_ms;
      series.windows[window_key] = state;
    }
    if (entry.coverage_lost) {
      if (entry.loss_from_ms >= 0) {
        recordLoss(series.windows[window_key], entry.loss_from_ms, entry.loss_to_ms);
      } else {
        recordLoss(series.windows[window_key], entry.first_timestamp_ms, entry.sample.timestamp_ms);
      }
    }
    propagateCoverageLoss(series, window_key);
    series.fixed_gap = series.fixed_gap || entry.fixed_gap;
  }
  prune(now_ms);
  enforceLimits();
  return true;
}

void MetricHistory::clearSource(const std::string& source_id) {
  for (auto it = series_.begin(); it != series_.end();) {
    if (it->second.source_id == source_id)
      it = series_.erase(it);
    else
      ++it;
  }
  recomputeCounts();
}

void MetricHistory::prune(int64_t now_ms) {
  const int64_t cutoff = now_ms > limits_.retention_ms ? now_ms - limits_.retention_ms : 0;
  for (auto it = series_.begin(); it != series_.end();) {
    Series& series = it->second;
    auto& points = series.raw_points;
    while (!points.empty() && points.front().timestamp_ms < cutoff) {
      series.raw_gap_before_ms =
          std::max(series.raw_gap_before_ms, points.front().timestamp_ms);
      points.pop_front();
    }
    auto& display = series.points;
    while (!display.empty() && display.front().sample.timestamp_ms < cutoff)
      display.pop_front();
    pruneFixed(series, cutoff);
    if (points.empty() && display.empty() && series.windows.empty())
      it = series_.erase(it);
    else
      ++it;
  }
  recomputeCounts();
}

std::vector<MetricSample> MetricHistory::query(const std::string& source_id,
                                               const std::string& metric,
                                               const std::vector<KeyValue>& labels,
                                               const ResolvedWindow& window) const {
  std::vector<MetricSample> out;
  for (const auto& item : series_) {
    const Series& series = item.second;
    if (series.source_id != source_id || series.metric != metric ||
        !labelsMatch(series.labels, labels))
      continue;
    for (const HistoryEntry& entry : series.points) {
      if (entry.sample.timestamp_ms >= window.from_ms &&
          entry.sample.timestamp_ms <= window.to_ms) {
        out.push_back(entry.sample);
      }
    }
  }
  std::stable_sort(out.begin(), out.end(), [](const MetricSample& left,
                                              const MetricSample& right) {
    return left.timestamp_ms < right.timestamp_ms;
  });
  return out;
}

AggregationResult MetricHistory::aggregate(const std::string& source_id,
                                           const std::string& metric,
                                           const std::vector<KeyValue>& labels,
                                           WindowKind window, int64_t now_ms,
                                           Aggregation aggregation,
                                           int32_t timezone_offset_minutes) const {
  const ResolvedWindow resolved = resolveWindow(window, now_ms, resetAt(source_id, metric),
                                                timezone_offset_minutes);
  if (window != WindowKind::Rolling24h) {
    std::vector<HistoryEntry> selected;
    for (const auto& item : series_) {
      const Series& series = item.second;
      if (series.source_id != source_id || series.metric != metric ||
          !labelsMatch(series.labels, labels))
        continue;
      const AggregationResult check = aggregateFixed(series, window, resolved, aggregation);
      if (!check.complete) return incompleteAggregate();
      if (check.sample_count == 0) continue;
      const WindowAccumulator& state = series.windows.at({window, resolved.from_ms});
      HistoryEntry entry;
      entry.sample.source_id = series.source_id;
      entry.sample.metric = series.metric;
      entry.sample.unit = series.unit;
      entry.sample.labels = series.labels;
      entry.sample.timestamp_ms = state.last_timestamp_ms;
      entry.sample.value = state.last_value;
      entry.count = state.count;
      entry.sum = state.sum;
      entry.min = state.min;
      entry.max = state.max;
      entry.first_value = state.first_value;
      entry.last_value = state.last_value;
      entry.first_timestamp_ms = state.first_timestamp_ms;
      entry.aggregated = true;
      selected.push_back(std::move(entry));
    }
    return aggregateHistoryEntries(selected, aggregation);
  }
  return aggregate(source_id, metric, labels, resolved, aggregation);
}

AggregationResult MetricHistory::aggregate(const std::string& source_id,
                                           const std::string& metric,
                                           const std::vector<KeyValue>& labels,
                                           const ResolvedWindow& window,
                                           Aggregation aggregation) const {
  std::vector<MetricSample> selected;
  for (const auto& item : series_) {
    const Series& series = item.second;
    if (series.source_id != source_id || series.metric != metric ||
        !labelsMatch(series.labels, labels))
      continue;
    if (series.raw_gap_before_ms > 0 && series.raw_gap_before_ms >= window.from_ms &&
        series.raw_gap_before_ms <= window.to_ms) {
      return incompleteAggregate();
    }
    for (const MetricSample& raw : series.raw_points) {
      if (raw.timestamp_ms >= window.from_ms && raw.timestamp_ms <= window.to_ms)
        selected.push_back(raw);
    }
  }
  return aggregateSamples(selected, aggregation);
}

std::vector<HistoryEntry> MetricHistory::entries(const std::string& source_id) const {
  std::vector<HistoryEntry> out;
  out.reserve(raw_point_count_);
  for (const auto& item : series_) {
    const Series& series = item.second;
    if (!source_id.empty() && series.source_id != source_id) continue;
    for (const MetricSample& raw : series.raw_points) {
      HistoryEntry entry;
      entry.sample = raw;
      entry.reset_at_ms = series.reset_at_ms;
      entry.raw_gap_before_ms = series.raw_gap_before_ms;
      entry.fixed_gap = series.fixed_gap;
      out.push_back(std::move(entry));
    }
    struct CoverageWindow {
      WindowKey key;
      bool coverage_lost = false;
      int64_t loss_from_ms = -1;
      int64_t loss_to_ms = -1;
    };
    struct Coverage {
      MetricSample sample;
      std::vector<CoverageWindow> windows;
      bool conflict = false;
    };
    std::map<int64_t, Coverage> coverage;
    for (const auto& window : series.windows) {
      const WindowAccumulator& state = window.second;
      for (const auto& point : state.samples) {
        Coverage& item = coverage[point.first];
        if (item.windows.empty()) {
          item.sample.source_id = series.source_id;
          item.sample.metric = series.metric;
          item.sample.unit = series.unit;
          item.sample.labels = series.labels;
          item.sample.timestamp_ms = point.first;
          item.sample.value = point.second;
        } else if (item.sample.value != point.second) {
          item.conflict = true;
        }
        CoverageWindow coverage_window{window.first, state.coverage_lost, state.loss_from_ms,
                                        state.loss_to_ms};
        if (coverage_window.coverage_lost && coverage_window.loss_from_ms < 0) {
          coverage_window.loss_from_ms = point.first;
          coverage_window.loss_to_ms = point.first;
        }
        item.windows.push_back(std::move(coverage_window));
      }
      if (state.samples.empty()) {
        HistoryEntry entry;
        entry.sample.source_id = series.source_id;
        entry.sample.metric = series.metric;
        entry.sample.unit = series.unit;
        entry.sample.labels = series.labels;
        entry.sample.timestamp_ms = state.last_timestamp_ms;
        entry.sample.value = state.last_value;
        entry.reset_at_ms = series.reset_at_ms;
        entry.count = state.count;
        entry.sum = state.sum;
        entry.min = state.min;
        entry.max = state.max;
        entry.first_value = state.first_value;
        entry.last_value = state.last_value;
        entry.first_timestamp_ms = state.first_timestamp_ms;
        entry.aggregated = true;
        entry.window_state = true;
        entry.window_kind = window.first.kind;
        entry.window_from_ms = window.first.from_ms;
        entry.raw_gap_before_ms = series.raw_gap_before_ms;
        entry.fixed_gap = series.fixed_gap;
        entry.coverage_lost = state.coverage_lost;
        entry.loss_from_ms = state.loss_from_ms;
        entry.loss_to_ms = state.loss_to_ms;
        if (entry.coverage_lost && entry.loss_from_ms < 0) {
          entry.loss_from_ms = entry.first_timestamp_ms;
          entry.loss_to_ms = entry.sample.timestamp_ms;
        }
        out.push_back(std::move(entry));
      }
    }
    for (const auto& item : coverage) {
      const Coverage& coverage_sample = item.second;
      HistoryEntry healthy;
      HistoryEntry lost;
      const auto initialize = [&](HistoryEntry& entry, bool coverage_lost) {
        entry.sample = coverage_sample.sample;
        entry.reset_at_ms = series.reset_at_ms;
        entry.count = 1;
        entry.sum = entry.sample.value;
        entry.min = entry.sample.value;
        entry.max = entry.sample.value;
        entry.first_value = entry.sample.value;
        entry.last_value = entry.sample.value;
        entry.first_timestamp_ms = entry.sample.timestamp_ms;
        entry.aggregated = true;
        entry.window_state = true;
        entry.coverage_sample = true;
        entry.raw_gap_before_ms = series.raw_gap_before_ms;
        entry.fixed_gap = series.fixed_gap;
        entry.coverage_lost = coverage_lost;
      };
      const auto extend_loss = [](HistoryEntry& entry, int64_t from_ms, int64_t to_ms) {
        if (from_ms < 0 || to_ms < 0) return;
        entry.loss_from_ms =
            entry.loss_from_ms < 0 ? from_ms : std::min(entry.loss_from_ms, from_ms);
        entry.loss_to_ms = entry.loss_to_ms < 0 ? to_ms : std::max(entry.loss_to_ms, to_ms);
      };
      initialize(healthy, false);
      initialize(lost, true);
      for (const auto& window : coverage_sample.windows) {
        const bool coverage_lost = window.coverage_lost || coverage_sample.conflict;
        HistoryEntry& entry = coverage_lost ? lost : healthy;
        if (entry.coverage_windows.empty()) {
          entry.window_kind = window.key.kind;
          entry.window_from_ms = window.key.from_ms;
        }
        entry.coverage_windows.emplace_back(window.key.kind, window.key.from_ms);
        if (coverage_lost) {
          extend_loss(entry, window.loss_from_ms, window.loss_to_ms);
          if (coverage_sample.conflict) {
            extend_loss(entry, entry.sample.timestamp_ms, entry.sample.timestamp_ms);
          }
        }
      }
      if (!healthy.coverage_windows.empty()) out.push_back(std::move(healthy));
      if (!lost.coverage_windows.empty()) out.push_back(std::move(lost));
    }
  }
  return out;
}

void MetricHistory::reconcileReset(const SourceResult& result) {
  for (auto& item : series_) {
    Series& series = item.second;
    if (series.source_id != result.source_id) continue;
    series.reset_at_ms = std::max(series.reset_at_ms, result.resetAtFor(series.metric));
  }
}

int64_t MetricHistory::resetAt(const std::string& source_id, const std::string& metric) const {
  int64_t reset = 0;
  for (const auto& item : series_) {
    if (item.second.source_id == source_id && item.second.metric == metric)
      reset = std::max(reset, item.second.reset_at_ms);
  }
  return reset;
}

}  // namespace eego::dashboard::pipeline
