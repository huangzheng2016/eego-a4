// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "data_pipeline/types.h"

namespace eego::dashboard::pipeline {

struct ResolvedWindow {
  int64_t from_ms = 0;
  int64_t to_ms = 0;
};

// reset_at is the provider-declared end of the current fixed window. It takes
// precedence over locally aligned fixed_5h/fixed_7d windows when positive.
ResolvedWindow resolveWindow(WindowKind kind, int64_t now_ms, int64_t reset_at_ms = 0,
                             int32_t timezone_offset_minutes = 0);

struct AggregationResult {
  bool valid = false;
  double value = 0.0;
  size_t sample_count = 0;
  int64_t first_timestamp_ms = 0;
  int64_t last_timestamp_ms = 0;
};

struct HistoryEntry;

AggregationResult aggregateSamples(const std::vector<MetricSample>& samples,
                                   Aggregation aggregation);
AggregationResult aggregateHistoryEntries(const std::vector<HistoryEntry>& entries,
                                          Aggregation aggregation);

struct HistoryEntry {
  MetricSample sample;
  int64_t reset_at_ms = 0;
  size_t count = 0;
  double sum = 0.0;
  double min = 0.0;
  double max = 0.0;
  double first_value = 0.0;
  double last_value = 0.0;
  int64_t first_timestamp_ms = 0;
  bool aggregated = false;
};

struct HistoryLimits {
  size_t max_series = 64;
  size_t max_points_per_series = 256;
  size_t downsample_points = 128;
  size_t max_total_points = 1024;
  int64_t retention_ms = 8LL * 24 * 60 * 60 * 1000;
};

void downsampleEntries(std::vector<HistoryEntry>& entries, size_t target_points);

class MetricHistory {
 public:
  explicit MetricHistory(HistoryLimits limits = {});

  bool append(const SourceResult& result, int64_t now_ms, std::string& error);
  bool replaceSource(const std::string& source_id, const std::vector<HistoryEntry>& entries,
                     int64_t now_ms, std::string& error);
  void clearSource(const std::string& source_id);
  void prune(int64_t now_ms);

  std::vector<MetricSample> query(const std::string& source_id, const std::string& metric,
                                  const std::vector<KeyValue>& labels,
                                  const ResolvedWindow& window) const;
  AggregationResult aggregate(const std::string& source_id, const std::string& metric,
                              const std::vector<KeyValue>& labels, WindowKind window,
                              int64_t now_ms, Aggregation aggregation,
                              int32_t timezone_offset_minutes = 0) const;
  // Raw samples for exact aggregation and SD persistence; query() returns display buckets.
  std::vector<HistoryEntry> entries(const std::string& source_id = "") const;
  int64_t resetAt(const std::string& source_id, const std::string& metric) const;

  size_t pointCount() const { return point_count_; }  // display buckets
  size_t rawPointCount() const { return raw_point_count_; }
  size_t seriesCount() const { return series_.size(); }
  const HistoryLimits& limits() const { return limits_; }

 private:
  struct Series {
    std::string source_id;
    std::string metric;
    std::vector<KeyValue> labels;
    int64_t reset_at_ms = 0;
    std::deque<MetricSample> raw_points;
    std::deque<HistoryEntry> points;
  };

  static std::string keyFor(const MetricSample& sample);
  bool appendEntry(HistoryEntry entry, std::string& error);
  void recomputeCounts();
  void enforceLimits();
  void downsample(Series& series);

  HistoryLimits limits_;
  std::map<std::string, Series> series_;
  size_t point_count_ = 0;
  size_t raw_point_count_ = 0;
};

}  // namespace eego::dashboard::pipeline
