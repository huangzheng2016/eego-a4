// SPDX-License-Identifier: GPL-3.0-or-later
#include "dashboard_ui/renderer.h"

#include <algorithm>
#include <cstring>
#include <new>

#include "dashboard_ui/text.h"
#include "dashboard_ui/update.h"
#include "panel_draw.h"

namespace eego::dashboard::ui {
namespace {

class Hash {
 public:
  void byte(uint8_t value) {
    value_ ^= value;
    value_ *= 1099511628211ULL;
  }
  void number(uint64_t value) {
    for (size_t i = 0; i < 8; ++i) byte(static_cast<uint8_t>(value >> (i * 8)));
  }
  void string(std::string_view value) {
    number(value.size());
    for (const unsigned char c : value) byte(c);
  }
  void real(double value) {
    uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "unexpected double size");
    std::memcpy(&bits, &value, sizeof(bits));
    number(bits);
  }
  uint64_t value() const { return value_; }

 private:
  uint64_t value_ = 1469598103934665603ULL;
};

void hashInterpolated(Hash& hash, const std::string& input, const VariableValues& variables) {
  std::string output;
  std::string error;
  if (interpolateVariables(input, variables, output, error)) {
    hash.string(output);
  } else {
    hash.string(error);
  }
}

void hashSample(Hash& hash, const MetricSample& sample) {
  hash.string(sample.source_id);
  hash.string(sample.metric);
  hash.number(sample.timestamp_ms);
  hash.real(sample.value);
  hash.string(sample.unit);
  for (const KeyValue& label : sample.labels) {
    hash.string(label.key);
    hash.string(label.value);
  }
}

void hashPanel(Hash& hash, const PanelSpec& panel) {
  hash.string(panel.id);
  hash.number(static_cast<uint8_t>(panel.kind));
  hash.number(panel.grid.x);
  hash.number(panel.grid.y);
  hash.number(panel.grid.w);
  hash.number(panel.grid.h);
  hash.string(panel.title);
  hash.number(panel.visible);
  hash.string(panel.source_id);
  hash.string(panel.metric);
  hash.string(panel.value_path);
  hash.string(panel.value_expression);
  hash.string(panel.time_window.from);
  hash.string(panel.time_window.to);
  hash.number(panel.refresh_ms);
  for (const Threshold& threshold : panel.thresholds) {
    hash.real(threshold.value);
    hash.number(static_cast<uint8_t>(threshold.severity));
    hash.string(threshold.label);
  }
  hash.string(panel.style.unit);
  hash.string(panel.style.variant);
  hash.number(panel.style.precision);
  hash.number(panel.style.line_width);
  hash.number(panel.style.show_legend);
  hash.number(panel.style.invert);
  for (const std::string& id : panel.linked_panel_ids) hash.string(id);
  for (const Action& action : panel.actions) {
    hash.string(action.id);
    hash.number(static_cast<uint8_t>(action.kind));
    hash.string(action.label);
    hash.string(action.target);
    hash.string(action.value);
    hash.number(action.confirm);
  }
  hash.string(panel.text.content);
  hash.number(panel.text.multiline);
  hash.number(panel.text.max_lines);
  hash.string(panel.image.asset_id);
  hash.string(panel.image.alt_text);
  hash.number(static_cast<uint8_t>(panel.image.fit));
  hash.number(static_cast<uint8_t>(panel.chart.kind));
  hash.number(panel.chart.show_points);
  hash.number(panel.chart.fill);
  hash.number(panel.chart.has_y_min);
  hash.number(panel.chart.has_y_max);
  hash.real(panel.chart.y_min);
  hash.real(panel.chart.y_max);
  for (const TableColumn& column : panel.columns) {
    hash.string(column.key);
    hash.string(column.title);
    hash.string(column.unit);
    hash.number(column.width);
  }
}

uint64_t fingerprint(const Dashboard& dashboard, const PanelSpec& panel,
                     const VariableValues& variables, uint64_t image_generation) {
  Hash hash;
  hashPanel(hash, panel);
  hashInterpolated(hash, panel.title, variables);
  if (panel.kind == PanelKind::Text || panel.kind == PanelKind::Card ||
      panel.kind == PanelKind::Alert) {
    hashInterpolated(hash, panel.text.content, variables);
  }
  if (panel.kind == PanelKind::Button) {
    for (const Action& action : panel.actions) hashInterpolated(hash, action.label, variables);
  }
  if (panel.kind == PanelKind::Card || panel.kind == PanelKind::Chart ||
      panel.kind == PanelKind::Table || (panel.kind == PanelKind::Image && !panel.value_path.empty())) {
    for (const MetricSample& sample : dashboard.samples) {
      const bool source_matches = panel.kind == PanelKind::Image && panel.source_id.empty()
                                      ? true
                                      : sample.source_id == panel.source_id;
      const bool metric_matches = panel.kind == PanelKind::Image && panel.metric.empty()
                                      ? true
                                      : sample.metric == panel.metric;
      if (source_matches && metric_matches) hashSample(hash, sample);
    }
  }
  if (panel.kind == PanelKind::Card &&
      (panel.style.variant == "dual" || panel.style.variant == "triple")) {
    for (const std::string& id : panel.linked_panel_ids) {
      for (const Page& page : dashboard.pages) {
        for (const PanelSpec& linked : page.panels) {
          if (linked.id != id) continue;
          hashPanel(hash, linked);
          hashInterpolated(hash, linked.title, variables);
          for (const MetricSample& sample : dashboard.samples) {
            if (sample.source_id == linked.source_id && sample.metric == linked.metric)
              hashSample(hash, sample);
          }
        }
      }
    }
  }
  if (panel.kind == PanelKind::Alert) {
    for (const Event& event : dashboard.events) {
      if (!alertEventMatches(dashboard, panel, event)) continue;
      hash.string(event.id);
      hash.string(event.rule_id);
      hash.string(event.source_id);
      hash.number(event.timestamp_ms);
      hash.number(static_cast<uint8_t>(event.severity));
      hash.number(static_cast<uint8_t>(event.state));
      hash.string(event.title);
      hash.string(event.message);
      hash.string(event.linked_panel_id);
      if (!panel.metric.empty()) {
        for (const AlertRule& rule : dashboard.alert_rules) {
          if (rule.id == event.rule_id && rule.metric == panel.metric) {
            hash.string(rule.id);
            hash.string(rule.metric);
          }
        }
      }
    }
  }
  if (panel.kind == PanelKind::Image) {
    hash.number(image_generation);
    const ImageDependency dependency = resolveImageDependency(dashboard, panel, variables);
    const Asset* assets[] = {dependency.base_asset, dependency.dynamic_asset};
    for (size_t i = 0; i < 2; ++i) {
      const Asset* asset = assets[i];
      if (asset == nullptr || (i == 1 && asset == dependency.base_asset)) continue;
      hash.string(asset->id);
      hash.number(static_cast<uint8_t>(asset->kind));
      hashInterpolated(hash, asset->uri, variables);
      hash.string(asset->media_type);
      hash.string(asset->sha256);
      hash.number(asset->size_bytes);
      hash.number(asset->cache_seconds);
    }
    if (dependency.field_sample != nullptr) hashSample(hash, *dependency.field_sample);
    hash.string(dependency.field_value);
    hash.string(dependency.uri);
    hash.number(dependency.valid);
  }
  return hash.value();
}

const Page* selectPage(const Dashboard& dashboard, const RenderOptions& options) {
  const Page* fallback = nullptr;
  for (const Page& page : dashboard.pages) {
    if (!page.visible) continue;
    if (fallback == nullptr) fallback = &page;
    if (!options.page_id.empty() && page.id == options.page_id) return &page;
    if (options.page_id.empty() && page.id == dashboard.default_page_id) return &page;
  }
  return options.page_id.empty() ? fallback : nullptr;
}

void drawPanel(PortraitCanvas& canvas, const Dashboard& dashboard, const PanelLayout& layout,
               const VariableValues& variables, ImagePipeline& images, const Rect& clip) {
  const PanelSpec& panel = *layout.panel;
  FontManager& font = FontManager::instance();
  canvas.fillRect(layout.bounds.x, layout.bounds.y, layout.bounds.w, layout.bounds.h, 1);
  canvas.drawRect(layout.bounds.x, layout.bounds.y, layout.bounds.w, layout.bounds.h, 0);
  if (panel.kind == PanelKind::Button) {
    drawButtonPanel(canvas, font, layout, variables);
    return;
  }

  Rect body = layout.content;
  if (!panel.title.empty() && !body.empty()) {
    std::string title;
    std::string error;
    if (!interpolateVariables(panel.title, variables, title, error)) title = "Invalid title";
    const int16_t title_height = font.uiFontHeight() + 5;
    drawTextFit(canvas, font, body.x, body.y, body.w, title, 1, true, 0);
    canvas.drawLine(body.x, body.y + title_height - 2, body.x + body.w - 1,
                    body.y + title_height - 2, 0);
    body.y += title_height;
    body.h -= title_height;
  }
  if (body.empty()) return;

  switch (panel.kind) {
    case PanelKind::Text:
      drawTextPanel(canvas, font, panel, body, variables);
      break;
    case PanelKind::Image: {
      const Rect image_clip = intersect(clip, body);
      if (!image_clip.empty()) {
        canvas.setClip(image_clip.x, image_clip.y, image_clip.w, image_clip.h);
        images.draw(canvas, dashboard, panel, body, variables);
        canvas.setClip(clip.x, clip.y, clip.w, clip.h);
      }
      break;
    }
    case PanelKind::Card:
      drawCardPanel(canvas, font, dashboard, panel, body, variables);
      break;
    case PanelKind::Table:
      drawTablePanel(canvas, font, dashboard, panel, body);
      break;
    case PanelKind::Alert:
      drawAlertPanel(canvas, font, dashboard, panel, body, variables);
      break;
    case PanelKind::Chart:
      drawChartPanel(canvas, font, dashboard, panel, body);
      break;
    case PanelKind::Button:
      break;
  }
}

}  // namespace

struct DashboardRenderer::Impl {
  std::string view_key;
  std::vector<PanelVersion> panels;
  std::vector<std::string> manual_dirty;
  uint8_t fast_refreshes = 0;
};

DashboardRenderer::DashboardRenderer() : impl_(new (std::nothrow) Impl) {}
DashboardRenderer::~DashboardRenderer() { delete impl_; }

void DashboardRenderer::invalidatePanel(std::string_view panel_id) {
  if (impl_ == nullptr || panel_id.empty()) return;
  for (const std::string& id : impl_->manual_dirty) {
    if (id == panel_id || id == "*") return;
  }
  if (impl_->manual_dirty.size() >= 72) {
    impl_->manual_dirty.clear();
    impl_->manual_dirty.emplace_back("*");
  } else {
    impl_->manual_dirty.emplace_back(panel_id);
  }
}

void DashboardRenderer::invalidateImage(std::string_view asset_id) {
  images_.invalidate(asset_id);
  ++image_generation_;
}

void DashboardRenderer::reset() {
  if (impl_ != nullptr) *impl_ = Impl{};
  images_.clear();
  ++image_generation_;
}

RenderResult DashboardRenderer::render(PortraitCanvas& canvas, const Dashboard& dashboard,
                                       const RenderOptions& options) {
  RenderResult result;
  result.page = selectPage(dashboard, options);
  if (impl_ == nullptr) return result;
  if (result.page == nullptr) {
    if (impl_->view_key != "<none>" || options.force_full) {
      canvas.clear();
      result.refresh_needed = true;
      result.refresh_mode = RefreshMode::Full;
      result.full_redraw = true;
      result.dirty_rects.push_back(kDashboardContent);
    }
    impl_->view_key = "<none>";
    impl_->panels.clear();
    impl_->fast_refreshes = 0;
    return result;
  }

  result.layout = buildPageLayout(*result.page, options.page_index);
  const std::string view_key = result.page->id + "#" + std::to_string(options.page_index);
  const bool same_view = !options.force_full && impl_->view_key == view_key;
  std::vector<uint64_t> fingerprints;
  fingerprints.reserve(result.layout.panels.size());
  for (const PanelLayout& panel : result.layout.panels) {
    fingerprints.push_back(
        fingerprint(dashboard, *panel.panel, options.variables, image_generation_));
  }
  UpdatePlan update = planUpdate(result.layout, fingerprints,
                                 same_view ? &impl_->panels : nullptr,
                                 impl_->manual_dirty, options.force_full);
  result.full_redraw = update.full_redraw;
  result.dirty_panels = update.dirty_panels;
  const DirtyRects& dirty = update.dirty;

  std::vector<bool> drawn(result.layout.panels.size(), false);
  if (result.full_redraw) {
    canvas.clear();
    for (size_t i = 0; i < result.layout.panels.size(); ++i) {
      const PanelLayout& panel = result.layout.panels[i];
      canvas.setClip(panel.bounds.x, panel.bounds.y, panel.bounds.w, panel.bounds.h);
      drawPanel(canvas, dashboard, panel, options.variables, images_, panel.bounds);
      drawn[i] = true;
    }
  } else {
    for (const Rect& region : dirty.values()) {
      canvas.fillRect(region.x, region.y, region.w, region.h, 1);
      for (size_t i = 0; i < result.layout.panels.size(); ++i) {
        const PanelLayout& panel = result.layout.panels[i];
        const Rect clip = intersect(region, panel.bounds);
        if (clip.empty()) continue;
        canvas.setClip(clip.x, clip.y, clip.w, clip.h);
        drawPanel(canvas, dashboard, panel, options.variables, images_, clip);
        drawn[i] = true;
      }
    }
  }
  canvas.resetClip();
  result.drawn_panels = static_cast<size_t>(std::count(drawn.begin(), drawn.end(), true));
  result.dirty_rects = dirty.values();
  result.refresh_needed = !dirty.empty();
  const uint32_t content_area = static_cast<uint32_t>(result.layout.content.w) *
                                result.layout.content.h;
  const bool full_refresh = result.full_redraw || update.structural ||
                            (content_area > 0 && dirty.coverage() * 10 > content_area * 6) ||
                            impl_->fast_refreshes >= 4;
  result.refresh_mode = full_refresh ? RefreshMode::Full : RefreshMode::Fast;
  if (result.refresh_needed) {
    if (full_refresh) {
      impl_->fast_refreshes = 0;
    } else {
      ++impl_->fast_refreshes;
    }
  }
  impl_->view_key = view_key;
  impl_->panels = std::move(update.current);
  impl_->manual_dirty.clear();
  result.image_cache_bytes = images_.cacheBytes();
  return result;
}

}  // namespace eego::dashboard::ui
