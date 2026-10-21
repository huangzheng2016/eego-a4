// SPDX-License-Identifier: GPL-3.0-or-later
#include "data_pipeline/history.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace eego::dashboard::pipeline {
namespace {

constexpr int64_t kHourMs = 60LL * 60 * 1000;
constexpr int64_t kDayMs = 24 * kHourMs;

int64_t floorDay(int64_t value) {
  int64_t quotient = value / kDayMs;
  if (value < 0 && value % kDayMs != 0) --quotient;
  return quotient;
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
  if ((kind == WindowKind::Fixed5h || kind == WindowKind::Fixed7d) && reset_at_ms > 0) {
    return {std::max<int64_t>(reset_at_ms - duration, 0), reset_at_ms};
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

void downsampleEntries(std::vector<HistoryEntry>& entries, size_t target_points) {
  if (target_points == 0 || entries.size() <= target_points) return;
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
    double average = 0.0;
    for (size_t i = begin; i < end; ++i)
      average += entries[i].sample.value / static_cast<double>(end - begin);
    combined.sample.value = average;
    reduced.push_back(std::move(combined));
  }
  entries = std::move(reduced);
}

MetricHistory::MetricHistory(HistoryLimits limits) : limits_(sanitize(limits)) {}

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

void MetricHistory::downsample(Series& series) {
  if (series.points.size() <= limits_.max_points_per_series) return;
  std::vector<HistoryEntry> points;
  points.reserve(series.points.size());
  for (const HistoryEntry& entry : series.points) points.push_back(entry);
  downsampleEntries(points, limits_.downsample_points);
  series.points.clear();
  for (HistoryEntry& point : points) series.points.push_back(std::move(point));
}

void MetricHistory::enforceLimits() {
  point_count_ = 0;
  for (const auto& item : series_) point_count_ += item.second.points.size();
  while (series_.size() > limits_.max_series) {
    auto oldest = series_.begin();
    for (auto it = std::next(series_.begin()); it != series_.end(); ++it) {
      const int64_t it_time =
          it->second.points.empty() ? 0 : it->second.points.back().sample.timestamp_ms;
      const int64_t oldest_time = oldest->second.points.empty()
                                      ? 0
                                      : oldest->second.points.back().sample.timestamp_ms;
      if (it_time < oldest_time) oldest = it;
    }
    series_.erase(oldest);
  }
  point_count_ = 0;
  for (const auto& item : series_) point_count_ += item.second.points.size();
  while (point_count_ > limits_.max_total_points && !series_.empty()) {
    auto oldest = series_.begin();
    for (auto it = std::next(series_.begin()); it != series_.end(); ++it) {
      if (it->second.points.empty()) {
        oldest = it;
        break;
      }
      if (!oldest->second.points.empty() &&
          it->second.points.front().sample.timestamp_ms <
              oldest->second.points.front().sample.timestamp_ms) {
        oldest = it;
      }
    }
    if (oldest->second.points.empty()) {
      series_.erase(oldest);
      continue;
    }
    oldest->second.points.pop_front();
    --point_count_;
    if (oldest->second.points.empty()) series_.erase(oldest);
  }
}

bool MetricHistory::append(const SourceResult& result, int64_t now_ms, std::string& error) {
  error.clear();
  for (MetricSample sample : result.metrics) {
    if (sample.source_id.empty()) sample.source_id = result.source_id;
    if (sample.source_id != result.source_id || !validSample(sample)) {
      error = "history rejected an invalid or cross-source metric sample";
      return false;
    }
    sample.labels = canonicalLabels(std::move(sample.labels));
    const std::string key = keyFor(sample);
    auto it = series_.find(key);
    if (it == series_.end()) {
      Series series;
      series.source_id = sample.source_id;
      series.metric = sample.metric;
      series.labels = sample.labels;
      it = series_.emplace(key, std::move(series)).first;
    }
    Series& series = it->second;
    series.reset_at_ms = result.resetAtFor(sample.metric);
    HistoryEntry entry;
    entry.sample = std::move(sample);
    entry.reset_at_ms = series.reset_at_ms;
    auto position = std::lower_bound(series.points.begin(), series.points.end(),
                                     entry.sample.timestamp_ms,
                                     [](const HistoryEntry& point, int64_t timestamp) {
                                       return point.sample.timestamp_ms < timestamp;
                                     });
    if (position != series.points.end() &&
        position->sample.timestamp_ms == entry.sample.timestamp_ms) {
      *position = std::move(entry);
    } else {
      series.points.insert(position, std::move(entry));
    }
    downsample(series);
  }
  prune(now_ms);
  enforceLimits();
  return true;
}

bool MetricHistory::replaceSource(const std::string& source_id,
                                  const std::vector<HistoryEntry>& entries, int64_t now_ms,
                                  std::string& error) {
  clearSource(source_id);
  SourceResult result;
  result.source_id = source_id;
  result.fetched_at_ms = now_ms;
  result.metrics.reserve(entries.size());
  for (const HistoryEntry& entry : entries) {
    result.metrics.push_back(entry.sample);
    if (entry.reset_at_ms > 0) result.reset_at_ms[entry.sample.metric] = entry.reset_at_ms;
  }
  if (!append(result, now_ms, error)) {
    clearSource(source_id);
    return false;
  }
  return true;
}

void MetricHistory::clearSource(const std::string& source_id) {
  for (auto it = series_.begin(); it != series_.end();) {
    if (it->second.source_id == source_id)
      it = series_.erase(it);
    else
      ++it;
  }
  point_count_ = 0;
  for (const auto& item : series_) point_count_ += item.second.points.size();
}

void MetricHistory::prune(int64_t now_ms) {
  const int64_t cutoff = now_ms > limits_.retention_ms ? now_ms - limits_.retention_ms : 0;
  for (auto it = series_.begin(); it != series_.end();) {
    auto& points = it->second.points;
    while (!points.empty() && points.front().sample.timestamp_ms < cutoff) points.pop_front();
    if (points.empty())
      it = series_.erase(it);
    else
      ++it;
  }
  point_count_ = 0;
  for (const auto& item : series_) point_count_ += item.second.points.size();
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
  return aggregateSamples(query(source_id, metric, labels, resolved), aggregation);
}

std::vector<HistoryEntry> MetricHistory::entries(const std::string& source_id) const {
  std::vector<HistoryEntry> out;
  out.reserve(point_count_);
  for (const auto& item : series_) {
    if (!source_id.empty() && item.second.source_id != source_id) continue;
    for (const HistoryEntry& entry : item.second.points) out.push_back(entry);
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
