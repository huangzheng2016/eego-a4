// SPDX-License-Identifier: GPL-3.0-or-later
#include "data_pipeline/history.h"

#include <algorithm>
#include <cmath>
#include <limits>

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
  return !sample.source_id.empty() && !sample.metric.empty() && sample.metric.size() <= 128 &&
         sample.timestamp_ms >= 0 && std::isfinite(sample.value) &&
         sample.labels.size() <= kMaxLabels;
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
    WindowAccumulator& state = series.windows[{kind, from}];
    if (state.count == 0) {
      state.sum = sample.value;
      state.min = sample.value;
      state.max = sample.value;
      state.first_value = sample.value;
      state.last_value = sample.value;
      state.first_timestamp_ms = sample.timestamp_ms;
      state.last_timestamp_ms = sample.timestamp_ms;
      state.count = 1;
      return;
    }
    state.count += 1;
    state.sum += sample.value;
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
    if (!std::isfinite(state.sum)) state.coverage_lost = true;
  };
  add(WindowKind::Today);
  add(WindowKind::Fixed5h);
  add(WindowKind::Fixed7d);
}

void MetricHistory::markFixedIncomplete(Series& series, const MetricSample& sample) {
  const auto mark = [&](WindowKind kind) {
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
    const auto state = series.windows.find({kind, from});
    if (state != series.windows.end()) state->second.coverage_lost = true;
  };
  mark(WindowKind::Today);
  mark(WindowKind::Fixed5h);
  mark(WindowKind::Fixed7d);
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

bool MetricHistory::appendEntry(HistoryEntry entry, std::string& error) {
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
  series.reset_at_ms = entry.reset_at_ms;
  if (series.unit.empty()) series.unit = entry.sample.unit;
  auto position = std::lower_bound(series.raw_points.begin(), series.raw_points.end(),
                                   entry.sample.timestamp_ms,
                                   [](const MetricSample& point, int64_t timestamp) {
                                     return point.timestamp_ms < timestamp;
                                   });
  if (position != series.raw_points.end() &&
      position->timestamp_ms == entry.sample.timestamp_ms) {
    if (position->value != entry.sample.value) markFixedIncomplete(series, *position);
    *position = std::move(entry.sample);
  } else {
    position = series.raw_points.insert(position, std::move(entry.sample));
    addFixed(series, *position);
  }
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
    if (!appendEntry(std::move(entry), error)) {
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
        entry.window_from_ms < 0 || entry.count == 0 || !std::isfinite(entry.sum) ||
        !std::isfinite(entry.min) || !std::isfinite(entry.max) ||
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
    series.reset_at_ms = entry.reset_at_ms;
    if (series.unit.empty()) series.unit = canonical.unit;
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
    series.windows[{entry.window_kind, entry.window_from_ms}] = state;
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
  std::vector<MetricSample> selected;
  for (const auto& item : series_) {
    const Series& series = item.second;
    if (series.source_id != source_id || series.metric != metric ||
        !labelsMatch(series.labels, labels))
      continue;
    if (series.raw_gap_before_ms > 0 && series.raw_gap_before_ms >= resolved.from_ms &&
        series.raw_gap_before_ms <= resolved.to_ms) {
      return incompleteAggregate();
    }
    for (const MetricSample& raw : series.raw_points) {
      if (raw.timestamp_ms >= resolved.from_ms && raw.timestamp_ms <= resolved.to_ms)
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
    for (const auto& window : series.windows) {
      const WindowAccumulator& state = window.second;
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
      entry.fixed_gap = series.fixed_gap;
      entry.coverage_lost = state.coverage_lost;
      out.push_back(std::move(entry));
    }
  }
  return out;
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
