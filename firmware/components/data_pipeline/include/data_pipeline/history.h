// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <utility>
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
  bool complete = true;  // false means the query exceeded coverage or had conflicting data
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
  bool window_state = false;
  bool coverage_sample = false;
  std::vector<std::pair<WindowKind, int64_t>> coverage_windows;
  WindowKind window_kind = WindowKind::Rolling24h;
  int64_t window_from_ms = 0;
  int64_t raw_gap_before_ms = 0;
  bool fixed_gap = false;
  bool coverage_lost = false;
};

struct HistoryLimits {
  size_t max_series = 64;
  size_t max_points_per_series = 256;
  size_t downsample_points = 128;
  size_t max_raw_points_per_series = 1441;  // inclusive rolling_24h at 1m cadence
  size_t max_total_points = 8192;           // five full rolling series plus headroom
  int64_t retention_ms = 8LL * 24 * 60 * 60 * 1000;
};

void downsampleEntries(std::vector<HistoryEntry>& entries, size_t target_points);

class MetricHistory {
 public:
  explicit MetricHistory(HistoryLimits limits = {}, int32_t timezone_offset_minutes = 0);

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
  // Raw samples and exact fixed-window state for SD persistence; query() returns display buckets.
  std::vector<HistoryEntry> entries(const std::string& source_id = "") const;
  int64_t resetAt(const std::string& source_id, const std::string& metric) const;

  size_t pointCount() const { return point_count_; }  // display buckets
  size_t rawPointCount() const { return raw_point_count_; }
  size_t seriesCount() const { return series_.size(); }
  const HistoryLimits& limits() const { return limits_; }

 private:
  struct WindowKey {
    WindowKind kind = WindowKind::Rolling24h;
    int64_t from_ms = 0;
    bool operator<(const WindowKey& other) const {
      return kind != other.kind ? kind < other.kind : from_ms < other.from_ms;
    }
  };

  struct WindowAccumulator {
    size_t count = 0;
    double sum = 0.0;
    double min = 0.0;
    double max = 0.0;
    double first_value = 0.0;
    double last_value = 0.0;
    int64_t first_timestamp_ms = 0;
    int64_t last_timestamp_ms = 0;
    bool coverage_lost = false;
    std::vector<std::pair<int64_t, double>> samples;
  };

  struct Series {
    std::string source_id;
    std::string metric;
    std::string unit;
    std::vector<KeyValue> labels;
    int64_t reset_at_ms = 0;
    int64_t raw_gap_before_ms = 0;
    bool fixed_gap = false;
    std::deque<MetricSample> raw_points;
    std::deque<HistoryEntry> points;
    std::map<WindowKey, WindowAccumulator> windows;
  };

  static std::string keyFor(const MetricSample& sample);
  bool appendEntry(HistoryEntry entry, std::string& error);
  void addFixed(Series& series, const MetricSample& sample);
  void addWindowSample(Series& series, WindowKind kind, int64_t from_ms,
                       const MetricSample& sample);
  void pruneFixed(Series& series, int64_t cutoff_ms);
  AggregationResult aggregateFixed(const Series& series, WindowKind kind,
                                   const ResolvedWindow& window, Aggregation aggregation) const;
  void recomputeCounts();
  void enforceLimits();
  void downsample(Series& series);

  HistoryLimits limits_;
  int32_t timezone_offset_minutes_ = 0;
  std::map<std::string, Series> series_;
  size_t point_count_ = 0;
  size_t raw_point_count_ = 0;
};

}  // namespace eego::dashboard::pipeline
