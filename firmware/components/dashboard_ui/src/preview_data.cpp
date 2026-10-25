// SPDX-License-Identifier: GPL-3.0-or-later
#include "dashboard_ui/preview_data.h"

#include <algorithm>
#include <set>
#include <string>

namespace eego::dashboard::ui {
namespace {

constexpr size_t kMaxPreviewSamples = 4096;
constexpr size_t kMaxPreviewEvents = 256;
constexpr size_t kDemoPoints = 24;
constexpr int64_t kDemoStepMs = 300000;

uint32_t hash32(const std::string& text) {
  uint32_t h = 2166136261u;
  for (const unsigned char c : text) h = (h ^ c) * 16777619u;
  return h;
}

// Port of web/src/model/series.ts demoSeries (same seed -> same walk).
std::vector<double> demoSeries(const std::string& seed, double lo, double hi) {
  uint32_t x = hash32(seed);
  if (x == 0) x = 1;
  auto rnd = [&x]() {
    x = ((x ^ (x >> 15)) * 2246822519u) ^ ((x ^ (x >> 13)) * 3266489917u);
    return static_cast<double>(x) / 4294967296.0;
  };
  double v = lo + (hi - lo) * (0.35 + rnd() * 0.3);
  std::vector<double> out;
  out.reserve(kDemoPoints);
  for (size_t i = 0; i < kDemoPoints; ++i) {
    v += (rnd() - 0.48) * (hi - lo) * 0.18;
    v = std::max(lo, std::min(hi, v));
    out.push_back(static_cast<double>(static_cast<int64_t>(v * 10 + (v >= 0 ? 0.5 : -0.5))) / 10);
  }
  return out;
}

bool metricPanel(const PanelSpec& panel) {
  return !panel.metric.empty() &&
         (panel.kind == PanelKind::Card || panel.kind == PanelKind::Chart ||
          panel.kind == PanelKind::Table);
}

}  // namespace

const char* previewDataName(PreviewData data) {
  switch (data) {
    case PreviewData::Bundle: return "bundle";
    case PreviewData::Live: return "live";
    case PreviewData::Mixed: return "mixed";
    case PreviewData::Placeholder: return "placeholder";
    case PreviewData::None: return "none";
  }
  return "none";
}

PreviewData fillPreviewData(Dashboard& preview, const Dashboard* live, int64_t now_ms) {
  const bool had_bundle = !preview.samples.empty() || !preview.events.empty();
  bool used_live = false;
  if (live != nullptr) {
    std::set<std::string> sampled;
    std::set<std::string> evented;
    for (const MetricSample& sample : preview.samples) sampled.insert(sample.source_id);
    for (const Event& event : preview.events) evented.insert(event.source_id);
    for (const Source& source : preview.sources) {
      if (sampled.count(source.id) == 0) {
        for (const MetricSample& sample : live->samples) {
          if (preview.samples.size() >= kMaxPreviewSamples) break;
          if (sample.source_id != source.id) continue;
          preview.samples.push_back(sample);
          used_live = true;
        }
      }
      if (evented.count(source.id) == 0) {
        for (const Event& event : live->events) {
          if (preview.events.size() >= kMaxPreviewEvents) break;
          if (event.source_id != source.id) continue;
          preview.events.push_back(event);
          used_live = true;
        }
      }
    }
  }
  if (!preview.samples.empty()) {
    if (had_bundle && used_live) return PreviewData::Mixed;
    return used_live ? PreviewData::Live : PreviewData::Bundle;
  }

  const int64_t end = now_ms > 0 ? now_ms / kDemoStepMs * kDemoStepMs : 0;
  static const char* const kNames[] = {"api", "db", "cache", "queue", "cdn", "auth", "mail", "dns"};
  std::set<std::pair<std::string, std::string>> done;
  for (const Page& page : preview.pages) {
    for (const PanelSpec& panel : page.panels) {
      if (!metricPanel(panel) || !done.insert({panel.source_id, panel.metric}).second) continue;
      const bool progress = panel.style.variant == "progress" || panel.style.unit == "%" ||
                            panel.style.unit == "percent";
      const std::vector<double> values =
          demoSeries(panel.id + panel.metric, progress ? 30 : 10, progress ? 90 : 99);
      for (size_t i = 0; i < values.size() && preview.samples.size() < kMaxPreviewSamples; ++i) {
        MetricSample sample;
        sample.source_id = panel.source_id;
        sample.metric = panel.metric;
        sample.value = values[i];
        sample.unit = panel.style.unit;
        sample.timestamp_ms = end - static_cast<int64_t>(values.size() - 1 - i) * kDemoStepMs;
        sample.labels.push_back({"name", kNames[i % 8]});
        preview.samples.push_back(std::move(sample));
      }
    }
  }
  if (!preview.samples.empty()) return PreviewData::Placeholder;
  if (!preview.events.empty()) return used_live ? (had_bundle ? PreviewData::Mixed : PreviewData::Live)
                                                : PreviewData::Bundle;
  return PreviewData::None;
}

}  // namespace eego::dashboard::ui
