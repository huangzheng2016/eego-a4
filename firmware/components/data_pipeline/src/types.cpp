// SPDX-License-Identifier: GPL-3.0-or-later
#include "data_pipeline/types.h"

#include <utility>

namespace eego::dashboard::pipeline {
namespace {

template <typename Enum, size_t N>
const char* enumName(Enum value, const std::pair<Enum, const char*> (&names)[N]) {
  for (const auto& entry : names) {
    if (entry.first == value) return entry.second;
  }
  return "unknown";
}

template <typename Enum, size_t N>
bool enumValue(std::string_view text, Enum& out,
               const std::pair<Enum, const char*> (&names)[N]) {
  for (const auto& entry : names) {
    if (text == entry.second) {
      out = entry.first;
      return true;
    }
  }
  return false;
}

constexpr std::pair<ResponseKind, const char*> kResponses[] = {
    {ResponseKind::GenericJson, "generic_json"},
    {ResponseKind::Prometheus, "prometheus"},
    {ResponseKind::StandardJson, "standard_json"},
    {ResponseKind::MetricsJson, "metrics_json"},
    {ResponseKind::EventsJson, "events_json"},
    {ResponseKind::Image, "image"},
    {ResponseKind::Text, "text"},
};
constexpr std::pair<WindowKind, const char*> kWindows[] = {
    {WindowKind::Today, "today"},
    {WindowKind::Rolling24h, "rolling_24h"},
    {WindowKind::Fixed5h, "fixed_5h"},
    {WindowKind::Fixed7d, "fixed_7d"},
};
constexpr std::pair<Aggregation, const char*> kAggregations[] = {
    {Aggregation::Sum, "sum"},   {Aggregation::Avg, "avg"},
    {Aggregation::Min, "min"},   {Aggregation::Max, "max"},
    {Aggregation::Delta, "delta"}, {Aggregation::Rate, "rate"},
};

}  // namespace

const char* responseKindName(ResponseKind kind) { return enumName(kind, kResponses); }
bool responseKindFromString(std::string_view text, ResponseKind& out) {
  return enumValue(text, out, kResponses);
}
const char* windowKindName(WindowKind kind) { return enumName(kind, kWindows); }
bool windowKindFromString(std::string_view text, WindowKind& out) {
  return enumValue(text, out, kWindows);
}
const char* aggregationName(Aggregation aggregation) {
  return enumName(aggregation, kAggregations);
}
bool aggregationFromString(std::string_view text, Aggregation& out) {
  return enumValue(text, out, kAggregations);
}

int64_t SourceResult::resetAtFor(std::string_view metric) const {
  const auto exact = reset_at_ms.find(std::string(metric));
  if (exact != reset_at_ms.end()) return exact->second;
  const auto wildcard = reset_at_ms.find("*");
  return wildcard == reset_at_ms.end() ? 0 : wildcard->second;
}

const std::string* HttpResponse::header(std::string_view lower_name) const {
  const auto it = headers.find(std::string(lower_name));
  return it == headers.end() ? nullptr : &it->second;
}

}  // namespace eego::dashboard::pipeline
