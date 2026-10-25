// SPDX-License-Identifier: GPL-3.0-or-later
// Panel bodies (card / chart / table / alert / button). The frame, padding and
// title row are drawn by renderer.cpp; see panel_style.h for the shared rules.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "dashboard_ui/panel_hit.h"
#include "dashboard_ui/text.h"
#include "panel_draw.h"
#include "panel_style.h"

namespace eego::dashboard::ui {
namespace {

using style::Align;
using style::kBody;
using style::kCaption;
using style::kDisplay;
using style::kHeadline;

constexpr size_t kMaxSeriesSamples = 2048;
constexpr size_t kMaxPlotPoints = 128;

std::vector<const MetricSample*> series(const Dashboard& dashboard, const PanelSpec& panel) {
  std::vector<const MetricSample*> out;
  if (panel.metric.empty()) return out;
  for (const MetricSample& sample : dashboard.samples) {
    if (sample.source_id == panel.source_id && sample.metric == panel.metric) {
      if (out.size() < kMaxSeriesSamples) out.push_back(&sample);
    }
  }
  std::stable_sort(out.begin(), out.end(), [](const MetricSample* lhs, const MetricSample* rhs) {
    return lhs->timestamp_ms < rhs->timestamp_ms;
  });
  return out;
}

std::vector<const MetricSample*> plotSeries(const Dashboard& dashboard, const PanelSpec& panel,
                                            size_t maximum = kMaxPlotPoints) {
  const std::vector<const MetricSample*> input = series(dashboard, panel);
  std::vector<const MetricSample*> out;
  if (input.empty() || maximum == 0) return out;
  if (input.size() == 1 || maximum == 1) {
    out.push_back(input.back());
    return out;
  }
  const size_t count = std::min(maximum, input.size());
  out.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    out.push_back(input[i * (input.size() - 1) / (count - 1)]);
  }
  return out;
}

const MetricSample* firstFiniteSample(const std::vector<const MetricSample*>& samples) {
  for (const MetricSample* sample : samples) {
    if (std::isfinite(sample->value)) return sample;
  }
  return nullptr;
}

void expandConstantRange(double& minimum, double& maximum) {
  if (maximum > minimum) return;
  const double center = minimum;
  const double margin = std::max(1.0, std::fabs(center) * 0.000001);
  minimum = center - margin;
  maximum = center + margin;
  if (std::isfinite(minimum) && std::isfinite(maximum) && maximum > minimum) return;
  if (center > 0) {
    minimum = center / 2;
    maximum = center;
  } else if (center < 0) {
    minimum = center;
    maximum = center / 2;
  } else {
    minimum = -1;
    maximum = 1;
  }
}

bool normalizedRatio(double value, double minimum, double maximum, double& ratio) {
  if (!std::isfinite(value) || !std::isfinite(minimum) || !std::isfinite(maximum) ||
      maximum <= minimum) {
    return false;
  }
  if (value <= minimum) {
    ratio = 0;
    return true;
  }
  if (value >= maximum) {
    ratio = 1;
    return true;
  }
  const double range = maximum - minimum;
  if (std::isfinite(range) && range > 0) {
    ratio = (value - minimum) / range;
  } else {
    const double scale = std::max(std::fabs(minimum), std::fabs(maximum));
    if (!std::isfinite(scale) || scale == 0) return false;
    const double low = minimum / scale;
    const double high = maximum / scale;
    const double scaled_range = high - low;
    if (!std::isfinite(scaled_range) || scaled_range <= 0) return false;
    ratio = (value / scale - low) / scaled_range;
  }
  if (!std::isfinite(ratio)) return false;
  ratio = std::clamp(ratio, 0.0, 1.0);
  return true;
}

std::string_view label(const MetricSample& sample, std::string_view key) {
  for (const KeyValue& item : sample.labels) {
    if (item.key == key) return item.value;
  }
  return {};
}

std::string_view unitFor(const PanelSpec& panel, const MetricSample& sample) {
  return panel.style.unit.empty() ? std::string_view(sample.unit) : std::string_view(panel.style.unit);
}

bool crossed(const PanelSpec& panel, const Threshold& threshold, double value) {
  return panel.style.invert ? value <= threshold.value : value >= threshold.value;
}

Severity severityFor(const PanelSpec& panel, double value) {
  Severity severity = Severity::Info;
  if (!std::isfinite(value)) return severity;
  for (const Threshold& threshold : panel.thresholds) {
    if (crossed(panel, threshold, value) &&
        static_cast<uint8_t>(threshold.severity) >= static_cast<uint8_t>(severity))
      severity = threshold.severity;
  }
  return severity;
}

// Label of the most severe crossed threshold, else the severity name.
std::string severityLabel(const PanelSpec& panel, double value, Severity severity) {
  std::string out;
  for (const Threshold& threshold : panel.thresholds) {
    if (crossed(panel, threshold, value) && threshold.severity == severity && !threshold.label.empty())
      out = threshold.label;
  }
  return out.empty() ? std::string(severityName(severity)) : out;
}

std::string statusText(const PanelSpec& panel, const MetricSample& sample) {
  std::string status(label(sample, "status"));
  if (status.empty()) status = label(sample, "state");
  if (status.empty()) {
    const Severity severity = severityFor(panel, sample.value);
    for (const Threshold& threshold : panel.thresholds) {
      if (crossed(panel, threshold, sample.value) && threshold.severity == severity &&
          !threshold.label.empty())
        status = threshold.label;
    }
  }
  if (status.empty()) {
    status = panel.thresholds.empty()
                 ? style::formatValueText(sample.value, unitFor(panel, sample), panel.style.precision)
                 : "OK";
  }
  return sanitizeText(status, false, 32);
}

std::string interpolatedText(const PanelSpec& panel, const VariableValues& variables) {
  std::string output;
  std::string error;
  if (!interpolateVariables(panel.text.content, variables, output, error)) return "Invalid text";
  return sanitizeText(output, true);
}

void drawStaticText(PortraitCanvas& canvas, FontManager& font, const PanelSpec& panel,
                    const Rect& bounds, const VariableValues& variables,
                    std::string_view empty_message = "No data") {
  const std::string content = interpolatedText(panel, variables);
  if (content.empty()) {
    style::emptyState(canvas, font, bounds, empty_message);
    return;
  }
  const int16_t line_height = static_cast<int16_t>(style::lineHeight(font) + 4);
  drawTextWrapped(canvas, font, bounds, content, kCaption, line_height,
                  std::min<size_t>(std::max<uint32_t>(panel.text.max_lines, 1), bounds.h / line_height), 0);
}

// ── Sparkline / delta (shared by cards and charts) ──────────────────────────

struct PlotPoint {
  int16_t x;
  int16_t y;
};

// Thick polyline: a lw x lw brush along each segment.
void polyline(PortraitCanvas& canvas, const std::vector<PlotPoint>& points, int16_t lw) {
  if (points.size() < 2) return;
  const int16_t lo = static_cast<int16_t>(-(lw - 1) / 2);
  const int16_t hi = static_cast<int16_t>(lo + lw - 1);
  for (size_t i = 1; i < points.size(); ++i) {
    for (int16_t dy = lo; dy <= hi; ++dy)
      for (int16_t dx = lo; dx <= hi; ++dx)
        canvas.drawLine(static_cast<int16_t>(points[i - 1].x + dx),
                        static_cast<int16_t>(points[i - 1].y + dy),
                        static_cast<int16_t>(points[i].x + dx),
                        static_cast<int16_t>(points[i].y + dy), 0);
  }
}

// Minimal trend line: no axes, 2 px line, dot on the latest value.
void drawSparkline(PortraitCanvas& canvas, const PanelSpec& panel,
                   const std::vector<const MetricSample*>& samples, const Rect& area) {
  if (area.w < 8 || area.h < 6 || samples.size() < 2) return;
  const MetricSample* first = firstFiniteSample(samples);
  if (first == nullptr) return;
  double minimum = first->value;
  double maximum = first->value;
  for (const MetricSample* sample : samples) {
    if (!std::isfinite(sample->value)) continue;
    minimum = std::min(minimum, sample->value);
    maximum = std::max(maximum, sample->value);
  }
  expandConstantRange(minimum, maximum);
  const Rect plot{area.x, static_cast<int16_t>(area.y + 3), static_cast<int16_t>(area.w - 4),
                  static_cast<int16_t>(area.h - 6)};
  std::vector<PlotPoint> points;
  const size_t step = std::max<size_t>(1, samples.size() / std::max<size_t>(1, plot.w / 2));
  for (size_t i = 0; i < samples.size(); i += step) {
    double ratio = 0.5;
    if (!normalizedRatio(samples[i]->value, minimum, maximum, ratio)) continue;
    if (panel.style.invert) ratio = 1.0 - ratio;
    points.push_back({static_cast<int16_t>(plot.x + i * (plot.w - 1) / (samples.size() - 1)),
                      static_cast<int16_t>(plot.y + plot.h - 1 - ratio * (plot.h - 1))});
  }
  polyline(canvas, points, 2);
  if (!points.empty()) canvas.fillCircle(points.back().x, points.back().y, 3, 0);
}

// "▲ +11 (+15.3%)" — arrow + signed change; the relative part is dropped first.
void drawDelta(PortraitCanvas& canvas, FontManager& font, int16_t x, int16_t y, int16_t w,
               double latest, double previous, std::string_view unit, uint32_t precision,
               int16_t scale = kCaption) {
  const double delta = latest - previous;
  if (!std::isfinite(delta) || w <= 0) return;
  const style::NumberText rounded = style::formatNumber(delta, unit, precision);
  const bool zero = delta == 0 || rounded.number.find_first_not_of("0.,-") == std::string::npos;
  int16_t cursor = x;
  if (!zero) {
    const int16_t size = static_cast<int16_t>(std::max<int>(5, style::ascent(font, scale) * 3 / 5));
    const int16_t top = static_cast<int16_t>(y + style::ascent(font, scale) - size);
    style::triangle(canvas, cursor, top, size, delta > 0);
    cursor = static_cast<int16_t>(cursor + size + 4);
  }
  const int16_t remaining = static_cast<int16_t>(x + w - cursor);
  std::string main = zero ? std::string("unchanged") : style::formatSigned(delta, unit, precision);
  if (!zero && measureText(font, main, scale) > remaining)
    main = style::formatSigned(delta, unit, precision, true);
  const style::NumberText unit_text = style::formatNumber(latest, unit, precision);
  if (!zero && !unit_text.symbol && previous != 0 && std::isfinite(previous)) {
    const double pct = delta / std::fabs(previous) * 100.0;
    const std::string relative = main + " (" + style::formatSigned(pct, "%", 1) + ")";
    if (measureText(font, relative, scale) <= remaining) main = relative;
  }
  style::text(canvas, font, cursor, y, remaining, main, scale, false);
}

// ── Cards ───────────────────────────────────────────────────────────────────

// Value block (+ delta footer, + sparkline when tall). Priority when space is
// short: sparkline, then the delta footer, then value size.
void drawValueCard(PortraitCanvas& canvas, FontManager& font, const PanelSpec& panel,
                   const std::vector<const MetricSample*>& samples, const Rect& bounds,
                   bool allow_spark = true) {
  const MetricSample& latest = *samples.back();
  const std::string_view unit = unitFor(panel, latest);
  const int16_t caption = style::lineHeight(font);
  const int16_t gap = 6;
  const bool has_previous = samples.size() > 1 && std::isfinite(latest.value) &&
                            std::isfinite(samples[samples.size() - 2]->value);
  const style::ValueFit bare = style::fitValue(font, latest.value, unit, panel.style.precision,
                                               bounds.w, bounds.h);
  style::ValueFit fit = bare;
  bool footer = false;
  if (has_previous) {
    const style::ValueFit with_footer =
        style::fitValue(font, latest.value, unit, panel.style.precision, bounds.w,
                        static_cast<int16_t>(bounds.h - caption - gap));
    if (with_footer.ok() && with_footer.tier <= bare.tier) {
      fit = with_footer;
      footer = true;
    }
  }
  if (!fit.ok()) {
    style::text(canvas, font, bounds.x, bounds.y, bounds.w,
                style::formatValueText(latest.value, unit, panel.style.precision, true), kCaption,
                true);
    return;
  }
  const int16_t value_h = style::tierHeight(font, fit.tier);
  const int16_t block_h = static_cast<int16_t>(value_h + (footer ? gap + caption : 0));
  const int16_t spare = static_cast<int16_t>(bounds.h - block_h);
  const bool spark = allow_spark && samples.size() >= 3 && spare >= caption * 3;
  // Always top-aligned so cards in a row share their value baseline.
  const int16_t top = bounds.y;
  style::drawValue(canvas, font, bounds.x, top, latest.value, unit, panel.style.precision, fit);
  if (footer) {
    drawDelta(canvas, font, bounds.x, static_cast<int16_t>(top + value_h + gap), bounds.w,
              latest.value, samples[samples.size() - 2]->value, unit, panel.style.precision);
  }
  if (spark) {
    const int16_t spark_h = static_cast<int16_t>(std::min<int>(spare - gap * 2, std::max<int>(caption * 3, bounds.h / 3)));
    drawSparkline(canvas, panel,
                  samples, Rect{bounds.x, static_cast<int16_t>(bounds.bottom() - spark_h), bounds.w, spark_h});
  }
}

// Emphasises the change: big signed delta with arrow, current value as caption.
void drawDeltaCard(PortraitCanvas& canvas, FontManager& font, const PanelSpec& panel,
                   const std::vector<const MetricSample*>& samples, const Rect& bounds) {
  const MetricSample& latest = *samples.back();
  const std::string_view unit = unitFor(panel, latest);
  const double previous = samples.size() > 1 ? samples[samples.size() - 2]->value : latest.value;
  const double delta = latest.value - previous;
  const int16_t caption = style::lineHeight(font);
  const int16_t gap = 6;
  const bool footer = bounds.h >= caption * 2 + gap;
  const int16_t avail_h = static_cast<int16_t>(bounds.h - (footer ? caption + gap : 0));
  // Leave room for the arrow (~ascent of the chosen tier) in the width budget.
  const bool zero = !std::isfinite(delta) || delta == 0;
  const std::string sign = std::isfinite(delta) && delta > 0 ? "+" : "";
  style::ValueFit fit;
  int16_t arrow = 0;
  for (int16_t tier = 0; tier < style::tierCount() && !fit.ok(); ++tier) {
    arrow = zero ? 0 : static_cast<int16_t>(std::max<int>(5, style::tierAscent(font, tier) * 3 / 5) + 6);
    fit = style::fitValue(font, delta, unit, panel.style.precision,
                          static_cast<int16_t>(bounds.w - arrow), avail_h, tier, tier, sign);
  }
  int16_t y = bounds.y;
  if (fit.ok()) {
    if (!zero) {
      const int16_t size = static_cast<int16_t>(arrow - 6);
      style::triangle(canvas, bounds.x,
                      static_cast<int16_t>(y + style::tierAscent(font, fit.tier) - size), size, delta > 0);
    }
    style::drawValue(canvas, font, static_cast<int16_t>(bounds.x + arrow), y, delta, unit,
                     panel.style.precision, fit, 0, sign);
    y = static_cast<int16_t>(y + style::tierHeight(font, fit.tier) + gap);
  }
  if (footer) {
    style::text(canvas, font, bounds.x, y, bounds.w,
                "now " + style::formatValueText(latest.value, unit, panel.style.precision), kCaption);
  }
}

void drawProgressCard(PortraitCanvas& canvas, FontManager& font, const PanelSpec& panel,
                      const MetricSample& sample, const Rect& bounds) {
  const std::string_view unit = panel.style.unit.empty() ? std::string_view("%") : std::string_view(panel.style.unit);
  const int16_t gap = 8;
  const int16_t bar_h = static_cast<int16_t>(std::clamp<int>(bounds.h / 6, 10, 18));
  const double clamped = std::isfinite(sample.value) ? std::clamp(sample.value, 0.0, 100.0) : 0.0;
  const auto drawBar = [&](int16_t x, int16_t y, int16_t w, int16_t h) {
    const int16_t r = static_cast<int16_t>(std::min<int>(4, h / 2));
    canvas.drawRoundRect(x, y, w, h, r, 0);
    const int16_t inner = static_cast<int16_t>(w - 6);
    const int16_t filled = static_cast<int16_t>(inner * clamped / 100.0 + 0.5);
    if (filled > 0) canvas.fillRect(static_cast<int16_t>(x + 3), static_cast<int16_t>(y + 3), filled,
                                    static_cast<int16_t>(h - 6), 0);
    // Threshold ticks above and below the bar.
    for (const Threshold& threshold : panel.thresholds) {
      if (!(threshold.value > 0 && threshold.value < 100)) continue;
      const int16_t tx = static_cast<int16_t>(x + 3 + inner * threshold.value / 100.0);
      canvas.fillRect(tx, static_cast<int16_t>(y - 4), 1, 3, 0);
      canvas.fillRect(tx, static_cast<int16_t>(y + h + 1), 1, 3, 0);
    }
  };
  // Short and wide: value left, bar right (keeps the value at body size).
  if (bounds.h < style::lineHeight(font, kBody) + gap + bar_h && bounds.w >= 120) {
    const style::ValueFit side = style::fitValue(font, sample.value, unit, panel.style.precision,
                                                 static_cast<int16_t>(bounds.w / 2), bounds.h);
    if (side.ok()) {
      const int16_t vy = static_cast<int16_t>(bounds.y + (bounds.h - style::tierHeight(font, side.tier)) / 2);
      style::drawValue(canvas, font, bounds.x, vy, sample.value, unit, panel.style.precision, side);
      const int16_t bx = static_cast<int16_t>(bounds.x + side.width + 12);
      const int16_t h = static_cast<int16_t>(std::min<int>(bar_h + 2, bounds.h));
      drawBar(bx, static_cast<int16_t>(bounds.y + (bounds.h - h) / 2), static_cast<int16_t>(bounds.right() - bx), h);
      return;
    }
  }
  const style::ValueFit fit = style::fitValue(font, sample.value, unit, panel.style.precision,
                                              bounds.w, static_cast<int16_t>(bounds.h - bar_h - gap));
  const int16_t value_h = fit.ok() ? style::tierHeight(font, fit.tier) : 0;
  int16_t y = bounds.y;
  if (fit.ok()) {
    style::drawValue(canvas, font, bounds.x, y, sample.value, unit, panel.style.precision, fit);
    y = static_cast<int16_t>(y + value_h + gap);
  }
  if (bounds.bottom() - y < 6) return;
  drawBar(bounds.x, y, bounds.w, static_cast<int16_t>(std::min<int>(bar_h, bounds.bottom() - y)));
}

// Severity chip: info outlined, warning double outline + marker, critical inverted.
void drawStatusChip(PortraitCanvas& canvas, FontManager& font, const Rect& chip,
                    const std::string& status, const std::string& detail, Severity severity) {
  if (chip.empty()) return;
  const int16_t r = 6;
  const bool inverted = severity == Severity::Critical;
  if (inverted) {
    canvas.fillRoundRect(chip.x, chip.y, chip.w, chip.h, r, 0);
  } else {
    canvas.drawRoundRect(chip.x, chip.y, chip.w, chip.h, r, 0);
    if (severity == Severity::Warning)
      canvas.drawRoundRect(static_cast<int16_t>(chip.x + 1), static_cast<int16_t>(chip.y + 1),
                           static_cast<int16_t>(chip.w - 2), static_cast<int16_t>(chip.h - 2), r - 1, 0);
  }
  const uint8_t ink = inverted ? 15 : 0;
  const int16_t caption = style::lineHeight(font);
  const Rect inner = inset(chip, 8);
  if (inner.empty()) return;
  // Status word in the largest native tier (up to the 48 px size) that fits.
  int16_t tier = -1;
  for (int16_t t = 1; t < style::tierCount(); ++t) {
    if (style::tierHeight(font, t) <= inner.h && style::tierTextWidth(font, t, status) <= inner.w - 4) {
      tier = t;
      break;
    }
  }
  if (tier < 0) tier = static_cast<int16_t>(style::tierCount() - 1);
  const int16_t status_h = style::tierHeight(font, tier);
  const bool with_detail = !detail.empty() && inner.h >= status_h + 6 + caption;
  const int16_t block = static_cast<int16_t>(status_h + (with_detail ? 6 + caption : 0));
  int16_t y = static_cast<int16_t>(inner.y + std::max(0, (inner.h - block) / 2));
  const int32_t status_w = std::min<int32_t>(inner.w, style::tierTextWidth(font, tier, status));
  if (style::tierTextWidth(font, tier, status) <= inner.w) {
    style::tierDrawText(canvas, font, tier, static_cast<int16_t>(inner.x + (inner.w - status_w) / 2), y,
                        status, ink);
  } else {
    style::text(canvas, font, inner.x, y, inner.w, status, kCaption, true, Align::Center, ink);
  }
  if (with_detail) {
    y = static_cast<int16_t>(y + status_h + 6);
    style::text(canvas, font, inner.x, y, inner.w, detail, kCaption, false, Align::Center, ink);
  }
}

void drawStatusCard(PortraitCanvas& canvas, FontManager& font, const PanelSpec& panel,
                    const MetricSample& sample, const Rect& bounds) {
  const Severity severity = severityFor(panel, sample.value);
  std::string status(label(sample, "status"));
  if (status.empty()) status = label(sample, "state");
  if (status.empty()) status = severityLabel(panel, sample.value, severity);
  const std::string detail =
      style::formatValueText(sample.value, unitFor(panel, sample), panel.style.precision);
  Rect chip = bounds;
  const int16_t max_h = static_cast<int16_t>(style::lineHeight(font, kHeadline) + style::lineHeight(font) + 28);
  if (chip.h > max_h) {
    chip.y = static_cast<int16_t>(chip.y + (chip.h - max_h) / 2);
    chip.h = max_h;
  }
  drawStatusChip(canvas, font, chip, sanitizeText(status, false, 32), detail, severity);
}

const PanelSpec* findDashboardPanel(const Dashboard& dashboard, std::string_view id) {
  for (const Page& page : dashboard.pages) {
    for (const PanelSpec& panel : page.panels) {
      if (panel.id == id) return &panel;
    }
  }
  return nullptr;
}

// Dual/triple: side-by-side mini cards split by hairlines (stacked when narrow).
void drawLinkedCards(PortraitCanvas& canvas, FontManager& font, const Dashboard& dashboard,
                     const PanelSpec& panel, const Rect& bounds,
                     const VariableValues& variables) {
  const size_t count = panel.style.variant == "dual" ? 2 : 3;
  std::vector<const PanelSpec*> cards{&panel};
  for (const std::string& id : panel.linked_panel_ids) {
    const PanelSpec* linked = findDashboardPanel(dashboard, id);
    if (linked == nullptr || linked->kind != PanelKind::Card) {
      style::emptyState(canvas, font, bounds, "Invalid links");
      return;
    }
    cards.push_back(linked);
  }
  if (cards.size() != count) {
    style::emptyState(canvas, font, bounds, "Invalid links");
    return;
  }

  const size_t columns = bounds.w >= static_cast<int16_t>(count * 110) ? count : 1;
  const size_t rows = (count + columns - 1) / columns;
  const int16_t gap = 16;
  const int16_t cell_w = static_cast<int16_t>((bounds.w - static_cast<int16_t>(columns - 1) * gap) /
                                              static_cast<int16_t>(columns));
  const int16_t cell_h = static_cast<int16_t>((bounds.h - static_cast<int16_t>(rows - 1) * gap) /
                                              static_cast<int16_t>(rows));
  if (cell_w <= 0 || cell_h <= 0) return;
  const int16_t caption = style::lineHeight(font);
  for (size_t i = 0; i < count; ++i) {
    const PanelSpec& card = *cards[i];
    Rect cell{static_cast<int16_t>(bounds.x + (i % columns) * (cell_w + gap)),
              static_cast<int16_t>(bounds.y + (i / columns) * (cell_h + gap)), cell_w, cell_h};
    if (i > 0) {
      if (columns > 1) {
        style::dottedVLine(canvas, static_cast<int16_t>(cell.x - gap / 2), cell.y,
                           static_cast<int16_t>(cell.bottom() - 1), 2);
      } else {
        style::dottedHLine(canvas, cell.x, static_cast<int16_t>(cell.right() - 1),
                           static_cast<int16_t>(cell.y - gap / 2), 2);
      }
    }
    std::string name = card.title.empty() ? card.metric : card.title;
    std::string interpolated;
    std::string error;
    if (!card.title.empty() && interpolateVariables(card.title, variables, interpolated, error))
      name = interpolated;
    const std::vector<const MetricSample*> samples = series(dashboard, card);
    int16_t badge_w = 0;
    if (!samples.empty() && cell.h >= caption * 3) {
      const Severity severity = severityFor(card, samples.back()->value);
      if (severity != Severity::Info) {
        badge_w = style::badge(canvas, font, cell.right(), cell.y, static_cast<int16_t>(cell.w / 2),
                               severityLabel(card, samples.back()->value, severity), severity);
      }
    }
    Rect value = cell;
    if (cell.h >= caption * 2 + 4) {
      style::text(canvas, font, cell.x, cell.y, static_cast<int16_t>(cell.w - badge_w - (badge_w ? 6 : 0)),
                  name, kCaption, true);
      value.y = static_cast<int16_t>(value.y + caption + 6);
      value.h = static_cast<int16_t>(value.h - caption - 6);
    }
    if (samples.empty()) {
      style::emptyState(canvas, font, value);
    } else {
      drawValueCard(canvas, font, card, samples, value, false);
    }
  }
}

// ── Charts ──────────────────────────────────────────────────────────────────

void dashedHLine(PortraitCanvas& canvas, int16_t x0, int16_t x1, int16_t y) {
  for (int16_t x = x0; x <= x1; x = static_cast<int16_t>(x + 7))
    canvas.fillRect(x, y, static_cast<int16_t>(std::min<int>(4, x1 - x + 1)), 1, 0);
}

void drawChartSeries(PortraitCanvas& canvas, FontManager& font, const PanelSpec& panel,
                     const std::vector<const MetricSample*>& samples, const Rect& bounds) {
  const MetricSample* first = samples.empty() ? nullptr : firstFiniteSample(samples);
  if (bounds.w < 8 || bounds.h < 8 || first == nullptr) {
    style::emptyState(canvas, font, bounds);
    return;
  }
  double minimum = panel.chart.has_y_min ? panel.chart.y_min : first->value;
  double maximum = panel.chart.has_y_max ? panel.chart.y_max : first->value;
  for (const MetricSample* sample : samples) {
    if (!std::isfinite(sample->value)) continue;
    if (!panel.chart.has_y_min) minimum = std::min(minimum, sample->value);
    if (!panel.chart.has_y_max) maximum = std::max(maximum, sample->value);
  }
  if (!std::isfinite(minimum) || !std::isfinite(maximum)) {
    style::emptyState(canvas, font, bounds);
    return;
  }
  const bool bar = panel.chart.kind == ChartKind::Bar;
  if (bar) {
    if (!panel.chart.has_y_min) minimum = std::min(0.0, minimum);
    if (!panel.chart.has_y_max) maximum = std::max(0.0, maximum);
  }
  expandConstantRange(minimum, maximum);

  const int16_t caption = style::lineHeight(font);
  const bool sparkline = panel.chart.kind == ChartKind::Sparkline || bounds.h < caption * 3 + 8;
  const bool axes = !sparkline;
  const bool y_labels = axes && panel.style.show_legend && bounds.h >= caption * 3 + 8 && bounds.w >= 120;
  const bool times = axes && bounds.h >= caption * 5 && samples.size() > 1 &&
                     samples.front()->timestamp_ms >= 1000000000000LL &&
                     samples.back()->timestamp_ms > samples.front()->timestamp_ms;
  const std::string_view unit = unitFor(panel, *samples.back());
  auto axisLabel = [&](double value) {
    style::NumberText text = style::formatNumber(value, unit, panel.style.precision, true);
    return text.symbol ? text.joined() : text.number;
  };

  Rect plot = bounds;
  if (times) plot.h = static_cast<int16_t>(plot.h - caption - 4);
  // Sparkline legend: "min X" / "max Y" under the line when there is room.
  const bool spark_legend = sparkline && panel.style.show_legend && bounds.h >= caption * 3 + 8 &&
                            bounds.w >= 120;
  if (spark_legend) {
    plot.h = static_cast<int16_t>(plot.h - caption - 4);
    const int16_t y = static_cast<int16_t>(bounds.bottom() - caption);
    const int16_t half = static_cast<int16_t>(bounds.w / 2);
    style::text(canvas, font, bounds.x, y, half, "min " + axisLabel(minimum), kCaption);
    style::text(canvas, font, static_cast<int16_t>(bounds.x + half), y, half, "max " + axisLabel(maximum),
                kCaption, false, Align::Right);
  }
  std::string max_label;
  std::string min_label;
  if (y_labels) {
    max_label = axisLabel(maximum);
    min_label = axisLabel(minimum);
    const int16_t gutter = static_cast<int16_t>(
        std::max(measureText(font, max_label, kCaption), measureText(font, min_label, kCaption)) + 6);
    if (gutter < plot.w / 3) {
      plot.x = static_cast<int16_t>(plot.x + gutter);
      plot.w = static_cast<int16_t>(plot.w - gutter);
    } else {
      max_label.clear();
      min_label.clear();
    }
    // Labels are centred on the extreme grid lines: keep half a line inside.
    plot.y = static_cast<int16_t>(plot.y + caption / 2);
    plot.h = static_cast<int16_t>(plot.h - caption);
  } else if (axes) {
    plot.y = static_cast<int16_t>(plot.y + 3);
    plot.h = static_cast<int16_t>(plot.h - 3);
  }
  const bool line_kind = !bar;
  if (line_kind) plot.w = static_cast<int16_t>(plot.w - 4);  // room for the end dot
  if (plot.w < 4 || plot.h < 4) {
    style::emptyState(canvas, font, bounds, {});
    return;
  }

  const auto y_for = [&](double value) {
    double ratio = 0.5;
    if (!normalizedRatio(value, minimum, maximum, ratio)) ratio = 0.5;
    if (panel.style.invert) ratio = 1.0 - ratio;
    return static_cast<int16_t>(plot.y + plot.h - 1 - ratio * (plot.h - 1));
  };
  const int16_t plot_right = static_cast<int16_t>(plot.right() - 1 + (line_kind ? 4 : 0));
  const bool zero_inside = minimum < 0 && maximum > 0;
  const int16_t baseline_y = zero_inside ? y_for(0) : y_for(panel.style.invert ? maximum : minimum);

  // Grid: dotted top line, solid baseline (zero when the range spans it).
  if (axes) {
    style::dottedHLine(canvas, plot.x, plot_right, y_for(panel.style.invert ? minimum : maximum), 3);
    if (zero_inside) {
      style::dottedHLine(canvas, plot.x, plot_right, y_for(panel.style.invert ? maximum : minimum), 3);
    }
    canvas.fillRect(plot.x, baseline_y, static_cast<int16_t>(plot_right - plot.x + 1), 1, 0);
    for (const Threshold& threshold : panel.thresholds) {
      if (threshold.value > minimum && threshold.value < maximum)
        dashedHLine(canvas, plot.x, plot_right, y_for(threshold.value));
    }
  }

  if (bar) {
    const int32_t n = static_cast<int32_t>(samples.size());
    const int32_t slot = std::max<int32_t>(1, plot.w / n);
    const int32_t gap = slot >= 8 ? std::max<int32_t>(2, slot / 5) : slot >= 3 ? 1 : 0;
    const int16_t zero_y = y_for(0);
    for (int32_t i = 0; i < n; ++i) {
      if (!std::isfinite(samples[i]->value)) continue;
      const int32_t x0 = plot.x + i * plot.w / n;
      const int32_t x1 = plot.x + (i + 1) * plot.w / n - gap;
      const int16_t y = y_for(samples[i]->value);
      canvas.fillRect(static_cast<int16_t>(x0), std::min(y, zero_y),
                      static_cast<int16_t>(std::max<int32_t>(1, x1 - x0)),
                      static_cast<int16_t>(std::max(1, std::abs(zero_y - y) + 1)), 0);
    }
  } else {
    std::vector<PlotPoint> points;
    points.reserve(samples.size());
    for (size_t i = 0; i < samples.size(); ++i) {
      if (!std::isfinite(samples[i]->value)) continue;
      points.push_back({static_cast<int16_t>(plot.x + (samples.size() == 1 ? plot.w / 2 : i * (plot.w - 1) / (samples.size() - 1))),
                        y_for(samples[i]->value)});
    }
    if (panel.chart.fill && points.size() > 1) {
      // 25 % ordered pattern: same look on fast (B/W) and full refreshes.
      for (size_t i = 1; i < points.size(); ++i) {
        const int16_t x0 = points[i - 1].x;
        const int16_t x1 = points[i].x;
        const int32_t span = std::max(1, x1 - x0);
        for (int32_t x = x0; x < x1 || (i + 1 == points.size() && x == x1); ++x) {
          const int16_t y = static_cast<int16_t>(points[i - 1].y + (points[i].y - points[i - 1].y) * (x - x0) / span);
          const int16_t top = std::min(y, baseline_y);
          const int16_t bottom = std::max(y, baseline_y);
          for (int16_t yy = top; yy <= bottom; ++yy)
            if (style::patternInk(static_cast<int16_t>(x), yy, 1)) canvas.drawPixel(static_cast<int16_t>(x), yy, 0);
        }
      }
    }
    int16_t lw = static_cast<int16_t>(std::clamp<uint32_t>(panel.style.line_width, 1, 4));
    if (lw < 2 && plot.h >= 32) lw = 2;
    polyline(canvas, points, lw);
    const bool dots = points.size() == 1 ||
                      (panel.chart.show_points && points.size() * 6 <= static_cast<size_t>(plot.w));
    if (dots) {
      for (const PlotPoint& point : points) canvas.fillCircle(point.x, point.y, 2, 0);
    }
    if (!points.empty()) {
      canvas.fillCircle(points.back().x, points.back().y, 4, 0);
      canvas.drawCircle(points.back().x, points.back().y, 5, 1);
    }
  }

  if (!max_label.empty()) {
    const int16_t gutter_w = static_cast<int16_t>(plot.x - bounds.x - 6);
    const int16_t top_y = y_for(panel.style.invert ? minimum : maximum);
    const int16_t bottom_y = y_for(panel.style.invert ? maximum : minimum);
    style::text(canvas, font, bounds.x, static_cast<int16_t>(top_y - caption / 2), gutter_w,
                panel.style.invert ? min_label : max_label, kCaption, false, Align::Right);
    style::text(canvas, font, bounds.x, static_cast<int16_t>(bottom_y - caption / 2), gutter_w,
                panel.style.invert ? max_label : min_label, kCaption, false, Align::Right);
  }
  if (times) {
    const int16_t y = static_cast<int16_t>(bounds.bottom() - caption);
    const std::string from = style::formatTime(samples.front()->timestamp_ms);
    const std::string to = style::formatTime(samples.back()->timestamp_ms);
    const int16_t half = static_cast<int16_t>((plot_right - plot.x + 1) / 2);
    style::text(canvas, font, plot.x, y, half, from, kCaption, false, Align::Left);
    style::text(canvas, font, static_cast<int16_t>(plot_right + 1 - half), y, half, to, kCaption,
                false, Align::Right);
  }
}

// ── Tables ──────────────────────────────────────────────────────────────────

bool isTimeKey(std::string_view key) {
  return key == "time" || key == "timestamp" || key == "timestamp_ms";
}

std::string tableValue(const MetricSample& sample, const TableColumn& column,
                       const PanelSpec& panel, bool compact = false) {
  if (column.key == "value") {
    const std::string_view unit = !column.unit.empty() ? std::string_view(column.unit) :
                                  (!panel.style.unit.empty() ? std::string_view(panel.style.unit) : std::string_view(sample.unit));
    return style::formatValueText(sample.value, unit, panel.style.precision, compact);
  }
  if (column.key == "metric") return sample.metric;
  if (column.key == "source" || column.key == "source_id") return sample.source_id;
  if (isTimeKey(column.key)) return style::formatTime(sample.timestamp_ms);
  return std::string(label(sample, column.key));
}

void drawDefaultTable(PortraitCanvas& canvas, FontManager& font, const PanelSpec& panel,
                      const std::vector<const MetricSample*>& samples, const Rect& bounds) {
  if (samples.empty()) {
    style::emptyState(canvas, font, bounds);
    return;
  }
  const int16_t caption = style::lineHeight(font);
  const int16_t row_height = static_cast<int16_t>(caption + 8);
  const bool header = panel.style.show_legend && bounds.h >= row_height * 2 + 2;
  const int16_t header_h = header ? static_cast<int16_t>(row_height + 2) : 0;
  size_t rows = std::min<size_t>(samples.size(), std::max(0, bounds.h - header_h) / row_height);
  if (rows == 0) return;
  const bool more = samples.size() > rows && rows >= 3;
  const size_t data_rows = more ? rows - 1 : rows;
  std::vector<TableColumn> columns = panel.columns;
  if (columns.empty()) columns.push_back({"value", "Value", "", 1});
  uint32_t weight = 0;
  for (const TableColumn& column : columns) weight += std::max<uint32_t>(1, column.width);
  std::vector<int16_t> xs;
  std::vector<int16_t> widths;
  int32_t x = bounds.x;
  for (size_t col = 0; col < columns.size(); ++col) {
    const int32_t width = col + 1 == columns.size()
                              ? bounds.right() - x
                              : static_cast<int32_t>(bounds.w) * std::max<uint32_t>(1, columns[col].width) / weight;
    xs.push_back(static_cast<int16_t>(x));
    widths.push_back(static_cast<int16_t>(width));
    x += width;
  }
  const auto align_of = [&](const TableColumn& column) {
    return column.key == "value" || isTimeKey(column.key) ? Align::Right : Align::Left;
  };
  const int16_t cell_pad = 4;
  int16_t y = bounds.y;
  if (header) {
    for (size_t col = 0; col < columns.size(); ++col) {
      const std::string title = columns[col].title.empty() ? columns[col].key : columns[col].title;
      style::text(canvas, font, static_cast<int16_t>(xs[col] + cell_pad), static_cast<int16_t>(y + 3),
                  static_cast<int16_t>(widths[col] - cell_pad * 2), title, kCaption, true,
                  align_of(columns[col]));
    }
    canvas.fillRect(bounds.x, static_cast<int16_t>(y + row_height), bounds.w, 2, 0);
    y = static_cast<int16_t>(y + header_h);
  }
  for (size_t row = 0; row < data_rows; ++row) {
    const MetricSample& sample = *samples[samples.size() - 1 - row];
    if (row % 2 == 1) style::fillGray(canvas, Rect{bounds.x, y, bounds.w, row_height}, style::kZebraGray);
    const Severity severity = severityFor(panel, sample.value);
    for (size_t col = 0; col < columns.size(); ++col) {
      const bool emphasise = columns[col].key == "value" && severity != Severity::Info;
      const int16_t cell_w = static_cast<int16_t>(widths[col] - cell_pad * 2);
      std::string cell = tableValue(sample, columns[col], panel);
      if (columns[col].key == "value" && measureText(font, cell, kCaption) > cell_w)
        cell = tableValue(sample, columns[col], panel, true);
      style::text(canvas, font, static_cast<int16_t>(xs[col] + cell_pad), static_cast<int16_t>(y + 4),
                  cell_w, cell, kCaption, emphasise, align_of(columns[col]));
    }
    if (severity != Severity::Info) {
      style::severityMarker(canvas, bounds.x, static_cast<int16_t>(y + (row_height - 6) / 2), 6, severity);
    }
    y = static_cast<int16_t>(y + row_height);
  }
  if (more) {
    style::text(canvas, font, bounds.x, static_cast<int16_t>(y + 4), bounds.w,
                "+" + std::to_string(samples.size() - data_rows) + " more", kCaption, false,
                Align::Right);
  }
}

void drawStatusGrid(PortraitCanvas& canvas, FontManager& font, const PanelSpec& panel,
                    const std::vector<const MetricSample*>& samples, const Rect& bounds) {
  if (samples.empty()) {
    style::emptyState(canvas, font, bounds);
    return;
  }
  const int16_t caption = style::lineHeight(font);
  const int16_t gap = 5;
  std::vector<const MetricSample*> items(samples.end() - std::min<size_t>(samples.size(), 32),
                                         samples.end());
  const size_t columns = std::clamp<size_t>(static_cast<size_t>((bounds.w + gap) / 110), 1,
                                            std::min<size_t>(items.size(), 6));
  const int16_t min_cell_h = static_cast<int16_t>(caption + 8);
  const size_t max_rows = std::max<size_t>(1, (bounds.h + gap) / (min_cell_h + gap));
  if (items.size() > max_rows * columns) {
    // Not everything fits: keep the worst states visible.
    std::stable_sort(items.begin(), items.end(), [&](const MetricSample* lhs, const MetricSample* rhs) {
      return static_cast<uint8_t>(severityFor(panel, lhs->value)) >
             static_cast<uint8_t>(severityFor(panel, rhs->value));
    });
    items.resize(max_rows * columns);
  }
  const size_t count = items.size();
  const size_t rows = (count + columns - 1) / columns;
  const int16_t cell_w = static_cast<int16_t>((bounds.w - static_cast<int16_t>(columns - 1) * gap) /
                                              static_cast<int16_t>(columns));
  const int16_t ideal_h = static_cast<int16_t>(caption + style::lineHeight(font, kBody) + 18);
  const int16_t cell_h = static_cast<int16_t>(std::min<int>(
      ideal_h, (bounds.h - static_cast<int16_t>(rows - 1) * gap) / static_cast<int16_t>(rows)));
  const int16_t inner_w = static_cast<int16_t>(cell_w - 12);
  const bool two_lines = cell_h - 12 >= caption * 2 + 4;
  // One status size for the whole grid: body only if every label fits.
  int16_t status_scale = two_lines && cell_h - 12 - caption - 4 >= style::lineHeight(font, kBody) ? kBody : kCaption;
  for (const MetricSample* sample : items) {
    if (measureText(font, statusText(panel, *sample), status_scale) > inner_w) status_scale = kCaption;
  }
  for (size_t i = 0; i < count; ++i) {
    const MetricSample& sample = *items[i];
    const Rect cell{static_cast<int16_t>(bounds.x + (i % columns) * (cell_w + gap)),
                    static_cast<int16_t>(bounds.y + (i / columns) * (cell_h + gap)), cell_w, cell_h};
    const Severity severity = severityFor(panel, sample.value);
    const bool inverted = severity == Severity::Critical;
    if (inverted) {
      canvas.fillRoundRect(cell.x, cell.y, cell.w, cell.h, 5, 0);
    } else {
      canvas.drawRoundRect(cell.x, cell.y, cell.w, cell.h, 5, 0);
      if (severity == Severity::Warning)
        canvas.drawRoundRect(static_cast<int16_t>(cell.x + 1), static_cast<int16_t>(cell.y + 1),
                             static_cast<int16_t>(cell.w - 2), static_cast<int16_t>(cell.h - 2), 4, 0);
    }
    const uint8_t ink = inverted ? 15 : 0;
    std::string name(label(sample, "name"));
    if (name.empty() && !sample.labels.empty()) name = sample.labels.front().value;
    if (name.empty()) name = sample.metric;
    const std::string status = statusText(panel, sample);
    const Rect inner = inset(cell, 6);
    if (inner.empty()) continue;
    if (two_lines) {
      style::text(canvas, font, inner.x, inner.y, inner.w, name, kCaption, false, Align::Left, ink);
      style::text(canvas, font, inner.x,
                  static_cast<int16_t>(inner.bottom() - style::lineHeight(font, status_scale)), inner.w,
                  status, status_scale, true, Align::Left, ink);
    } else {
      const int16_t status_w = static_cast<int16_t>(std::min<int32_t>(inner.w / 2, measureText(font, status, kCaption) + 1));
      const int16_t y = static_cast<int16_t>(inner.y + (inner.h - caption) / 2);
      style::text(canvas, font, inner.x, y, static_cast<int16_t>(inner.w - status_w - 4), name, kCaption,
                  false, Align::Left, ink);
      style::text(canvas, font, static_cast<int16_t>(inner.right() - status_w), y, status_w, status,
                  kCaption, true, Align::Right, ink);
    }
  }
}

void drawHeatmap(PortraitCanvas& canvas, FontManager& font, const PanelSpec& panel,
                 const std::vector<const MetricSample*>& samples, const Rect& bounds) {
  const MetricSample* first = samples.empty() ? nullptr : firstFiniteSample(samples);
  if (first == nullptr) {
    style::emptyState(canvas, font, bounds);
    return;
  }
  const size_t count = std::min<size_t>(samples.size(), 64);
  double minimum = first->value;
  double maximum = first->value;
  for (size_t i = samples.size() - count; i < samples.size(); ++i) {
    if (!std::isfinite(samples[i]->value)) continue;
    minimum = std::min(minimum, samples[i]->value);
    maximum = std::max(maximum, samples[i]->value);
  }
  const double label_min = minimum;
  const double label_max = maximum;
  expandConstantRange(minimum, maximum);
  const int16_t caption = style::lineHeight(font);
  const bool legend = bounds.h >= caption * 4 && bounds.w >= 160;
  Rect grid = bounds;
  if (legend) grid.h = static_cast<int16_t>(grid.h - caption - 8);
  // Columns so cells come out roughly square.
  size_t columns = 1;
  while (columns < count) {
    const size_t rows = (count + columns - 1) / columns;
    if (static_cast<int32_t>(grid.w) * static_cast<int32_t>(rows) <=
        static_cast<int32_t>(grid.h) * static_cast<int32_t>(columns))
      break;
    ++columns;
  }
  const size_t rows = (count + columns - 1) / columns;
  const int16_t gap = 3;
  int16_t cell = static_cast<int16_t>(std::min<int>((grid.w + gap) / static_cast<int>(columns),
                                                    (grid.h + gap) / static_cast<int>(rows)));
  if (cell - gap < 3) {
    style::emptyState(canvas, font, bounds, {});
    return;
  }
  const int16_t width = static_cast<int16_t>(cell * columns - gap);
  const int16_t x0 = static_cast<int16_t>(grid.x + (grid.w - width) / 2);
  for (size_t i = 0; i < count; ++i) {
    const MetricSample& sample = *samples[samples.size() - count + i];
    const Rect box{static_cast<int16_t>(x0 + (i % columns) * cell),
                   static_cast<int16_t>(grid.y + (i / columns) * cell),
                   static_cast<int16_t>(cell - gap), static_cast<int16_t>(cell - gap)};
    double ratio = 0;
    if (!normalizedRatio(sample.value, minimum, maximum, ratio)) {
      style::dottedHLine(canvas, box.x, static_cast<int16_t>(box.right() - 1),
                         static_cast<int16_t>(box.y + box.h / 2), 2);
      continue;
    }
    if (panel.style.invert) ratio = 1.0 - ratio;
    const uint8_t level = static_cast<uint8_t>(std::lround(ratio * 4));
    canvas.drawRect(box.x, box.y, box.w, box.h, 0);
    style::fillPattern(canvas, inset(box, 1), level);
  }
  if (legend) {
    const int16_t y = static_cast<int16_t>(grid.y + rows * cell - gap + 8);
    const std::string_view unit = unitFor(panel, *samples.back());
    const std::string low = style::formatValueText(label_min, unit, panel.style.precision, true);
    const std::string high = style::formatValueText(label_max, unit, panel.style.precision, true);
    int16_t x = x0;
    x = static_cast<int16_t>(x + style::text(canvas, font, x, y, 120, low, kCaption) + 6);
    const int16_t swatch = static_cast<int16_t>(caption - 4);
    for (uint8_t level = 0; level <= 4; ++level) {
      const Rect box{x, static_cast<int16_t>(y + 2), swatch, swatch};
      canvas.drawRect(box.x, box.y, box.w, box.h, 0);
      style::fillPattern(canvas, inset(box, 1), level);
      x = static_cast<int16_t>(x + swatch + 2);
    }
    style::text(canvas, font, static_cast<int16_t>(x + 4), y, 120, high, kCaption);
  }
}

void drawTimeline(PortraitCanvas& canvas, FontManager& font, const PanelSpec& panel,
                  const std::vector<const MetricSample*>& samples, const Rect& bounds) {
  if (samples.empty()) {
    style::emptyState(canvas, font, bounds);
    return;
  }
  const size_t count = std::min<size_t>(samples.size(), 32);
  const int16_t caption = style::lineHeight(font);
  const int16_t marker = static_cast<int16_t>(std::clamp<int>(bounds.h / 4, 6, 14));
  size_t warnings = 0;
  size_t criticals = 0;
  for (size_t i = samples.size() - count; i < samples.size(); ++i) {
    const Severity severity = severityFor(panel, samples[i]->value);
    warnings += severity == Severity::Warning;
    criticals += severity == Severity::Critical;
  }
  const bool summary = bounds.h >= caption * 2 + marker * 2 + 12;
  const bool times = bounds.h >= caption + marker * 2 + 8 &&
                     samples[samples.size() - count]->timestamp_ms >= 1000000000000LL;
  int16_t top = bounds.y;
  if (summary) {
    std::string text;
    if (criticals) text += std::to_string(criticals) + " critical";
    if (warnings) text += (text.empty() ? "" : ", ") + std::to_string(warnings) + " warning";
    if (text.empty()) text = "All clear";
    style::text(canvas, font, bounds.x, top, bounds.w, text, kCaption, false);
    top = static_cast<int16_t>(top + caption + 8);
  }
  // Axis right under the summary (or centred when the panel is a single strip).
  const int16_t y = summary ? static_cast<int16_t>(top + marker / 2 + 2)
                            : static_cast<int16_t>(bounds.y + (bounds.h - (times ? caption + 4 : 0)) / 2);
  const int16_t left = static_cast<int16_t>(bounds.x + marker / 2);
  const int16_t span = static_cast<int16_t>(bounds.w - marker - 1);
  canvas.fillRect(bounds.x, y, bounds.w, 1, 0);
  for (size_t i = 0; i < count; ++i) {
    const MetricSample& sample = *samples[samples.size() - count + i];
    const int16_t x = static_cast<int16_t>(left + (count == 1 ? span / 2 : i * span / (count - 1)));
    const Severity severity = severityFor(panel, sample.value);
    if (severity == Severity::Info) {
      canvas.fillRect(x, static_cast<int16_t>(y - 3), 1, 7, 0);
    } else {
      // White halo keeps the axis from running through the marker.
      canvas.fillRect(static_cast<int16_t>(x - marker / 2 - 1), static_cast<int16_t>(y - marker / 2 - 1),
                      static_cast<int16_t>(marker + 2), static_cast<int16_t>(marker + 2), 1);
      style::severityMarker(canvas, static_cast<int16_t>(x - marker / 2), static_cast<int16_t>(y - marker / 2),
                            marker, severity);
    }
  }
  if (times) {
    const int16_t ty = static_cast<int16_t>(y + marker / 2 + 4);
    const int16_t half = static_cast<int16_t>(bounds.w / 2);
    style::text(canvas, font, bounds.x, ty, half,
                style::formatTime(samples[samples.size() - count]->timestamp_ms), kCaption);
    style::text(canvas, font, static_cast<int16_t>(bounds.x + half), ty, half,
                style::formatTime(samples.back()->timestamp_ms), kCaption, false, Align::Right);
  }
}

}  // namespace

HeaderAccessory panelHeaderAccessory(const Dashboard& dashboard, const PanelSpec& panel) {
  HeaderAccessory out;
  switch (panel.kind) {
    case PanelKind::Card: {
      const std::string& variant = panel.style.variant;
      if (variant == "status" || variant == "dual" || variant == "triple") break;
      const std::vector<const MetricSample*> samples = series(dashboard, panel);
      if (samples.empty()) break;
      const double value = samples.back()->value;
      const Severity severity = severityFor(panel, value);
      if (severity == Severity::Info) break;
      out.text = severityLabel(panel, value, severity);
      out.severity = severity;
      out.badge = true;
      break;
    }
    case PanelKind::Chart: {
      const std::vector<const MetricSample*> samples = series(dashboard, panel);
      if (samples.empty() || !std::isfinite(samples.back()->value)) break;
      const MetricSample& latest = *samples.back();
      out.text = style::formatValueText(latest.value, unitFor(panel, latest), panel.style.precision, true);
      out.severity = severityFor(panel, latest.value);
      out.badge = out.severity != Severity::Info;
      break;
    }
    case PanelKind::Alert: {
      size_t active = 0;
      Severity worst = Severity::Info;
      for (const Event* event : alertPanelEvents(dashboard, panel)) {
        if (event->state != EventState::Active) continue;
        ++active;
        if (static_cast<uint8_t>(event->severity) > static_cast<uint8_t>(worst)) worst = event->severity;
      }
      if (active == 0) break;
      out.text = std::to_string(active) + " active";
      out.severity = worst;
      out.badge = true;
      break;
    }
    case PanelKind::Table: {
      if (panel.style.variant != "status_grid") break;
      const std::vector<const MetricSample*> samples = series(dashboard, panel);
      size_t issues = 0;
      Severity worst = Severity::Info;
      for (size_t i = samples.size() - std::min<size_t>(samples.size(), 32); i < samples.size(); ++i) {
        const Severity severity = severityFor(panel, samples[i]->value);
        if (severity == Severity::Info) continue;
        ++issues;
        if (static_cast<uint8_t>(severity) > static_cast<uint8_t>(worst)) worst = severity;
      }
      if (issues == 0) {
        if (!samples.empty()) out.text = "All OK";
        break;
      }
      out.text = std::to_string(issues) + (issues == 1 ? " issue" : " issues");
      out.severity = worst;
      out.badge = true;
      break;
    }
    default:
      break;
  }
  return out;
}

void drawCardPanel(PortraitCanvas& canvas, FontManager& font, const Dashboard& dashboard,
                   const PanelSpec& panel, const Rect& bounds,
                   const VariableValues& variables) {
  if (panel.style.variant == "dual" || panel.style.variant == "triple") {
    drawLinkedCards(canvas, font, dashboard, panel, bounds, variables);
    return;
  }
  const std::vector<const MetricSample*> samples = series(dashboard, panel);
  if (samples.empty()) {
    drawStaticText(canvas, font, panel, bounds, variables);
    return;
  }
  if (panel.style.variant == "progress") {
    drawProgressCard(canvas, font, panel, *samples.back(), bounds);
  } else if (panel.style.variant == "status") {
    drawStatusCard(canvas, font, panel, *samples.back(), bounds);
  } else if (panel.style.variant == "delta") {
    drawDeltaCard(canvas, font, panel, samples, bounds);
  } else {
    drawValueCard(canvas, font, panel, samples, bounds);
  }
}

void drawChartPanel(PortraitCanvas& canvas, FontManager& font, const Dashboard& dashboard,
                    const PanelSpec& panel, const Rect& bounds) {
  drawChartSeries(canvas, font, panel, plotSeries(dashboard, panel), bounds);
}

void drawTablePanel(PortraitCanvas& canvas, FontManager& font, const Dashboard& dashboard,
                    const PanelSpec& panel, const Rect& bounds) {
  const std::vector<const MetricSample*> samples = series(dashboard, panel);
  if (panel.style.variant == "status_grid") {
    drawStatusGrid(canvas, font, panel, samples, bounds);
  } else if (panel.style.variant == "heatmap") {
    drawHeatmap(canvas, font, panel, samples, bounds);
  } else if (panel.style.variant == "timeline") {
    drawTimeline(canvas, font, panel, samples, bounds);
  } else {
    drawDefaultTable(canvas, font, panel, samples, bounds);
  }
}

bool alertEventMatches(const Dashboard& dashboard, const PanelSpec& panel, const Event& event) {
  if (!panel.source_id.empty() && event.source_id != panel.source_id) return false;
  if (!panel.linked_panel_ids.empty() && event.linked_panel_id != panel.id) {
    bool linked = false;
    for (const std::string& id : panel.linked_panel_ids) linked = linked || id == event.linked_panel_id;
    if (!linked) return false;
  }
  if (!panel.metric.empty()) {
    bool metric_matches = false;
    for (const AlertRule& rule : dashboard.alert_rules) {
      if (rule.id == event.rule_id && rule.metric == panel.metric) metric_matches = true;
    }
    if (!metric_matches) return false;
  }
  return true;
}

bool alertEventBefore(const Event& lhs, const Event& rhs) {
  const bool lhs_active = lhs.state == EventState::Active;
  const bool rhs_active = rhs.state == EventState::Active;
  if (lhs_active != rhs_active) return lhs_active;
  if (lhs_active && lhs.severity != rhs.severity)
    return static_cast<uint8_t>(lhs.severity) > static_cast<uint8_t>(rhs.severity);
  return lhs.timestamp_ms > rhs.timestamp_ms;
}

void orderAlertEvents(std::vector<const Event*>& events) {
  std::stable_sort(events.begin(), events.end(),
                   [](const Event* lhs, const Event* rhs) { return alertEventBefore(*lhs, *rhs); });
  if (events.size() > kAlertListMax) events.resize(kAlertListMax);
}

std::vector<const Event*> alertPanelEvents(const Dashboard& dashboard, const PanelSpec& panel) {
  std::vector<const Event*> events;
  for (const Event& event : dashboard.events) {
    if (alertEventMatches(dashboard, panel, event)) events.push_back(&event);
  }
  orderAlertEvents(events);
  return events;
}

PanelChrome panelChrome(FontManager& font, const PanelSpec& panel, const Rect& frame) {
  PanelChrome out;
  out.body = inset(frame, style::kPad);
  const int16_t caption = style::lineHeight(font);
  // Very short panels give the title row up so the value keeps its space.
  // Dual/triple cards label each cell themselves (the first cell uses this title).
  const bool linked_cards = panel.kind == PanelKind::Card &&
                            (panel.style.variant == "dual" || panel.style.variant == "triple");
  if (panel.title.empty() || linked_cards || out.body.empty() ||
      out.body.h < caption * 2 + style::kTitleGap) {
    return out;
  }
  const int16_t row_h = std::max<int16_t>(caption, style::badgeHeight(font));
  out.title = true;
  out.title_row = {out.body.x, out.body.y, out.body.w, row_h};
  const int16_t header_h = static_cast<int16_t>(row_h + style::kTitleGap);
  out.body.y = static_cast<int16_t>(out.body.y + header_h);
  out.body.h = static_cast<int16_t>(out.body.h - header_h);
  return out;
}

AlertListLayout layoutAlertList(FontManager& font, const Dashboard& dashboard,
                                const PanelSpec& panel, const Rect& body) {
  AlertListLayout out;
  out.events = alertPanelEvents(dashboard, panel);
  if (out.events.empty() || body.empty()) return out;
  const int16_t caption = style::lineHeight(font);
  const int16_t single = static_cast<int16_t>(caption + 10);
  const int16_t twin = static_cast<int16_t>(caption * 2 + 12);
  const size_t fit_twin = body.h / twin;
  out.two_lines = fit_twin >= std::min<size_t>(out.events.size(), 4);
  out.row_height = out.two_lines ? twin : single;
  size_t rows = std::min<size_t>(out.events.size(), std::max<int>(0, body.h) / out.row_height);
  if (rows == 0) return out;
  out.more = out.events.size() > rows && rows >= 2 &&
             body.h - static_cast<int32_t>(rows) * out.row_height < caption + 4;
  if (out.more) --rows;
  out.rows.reserve(rows);
  for (size_t i = 0; i < rows; ++i) {
    out.rows.push_back({body.x, static_cast<int16_t>(body.y + i * out.row_height), body.w,
                        out.row_height});
  }
  if (out.more) {
    const int16_t y = static_cast<int16_t>(body.y + rows * out.row_height);
    out.more_bounds = {body.x, y, body.w, static_cast<int16_t>(body.bottom() - y)};
  }
  return out;
}

int alertRowAt(FontManager& font, const Dashboard& dashboard, const PanelLayout& panel,
               int16_t x, int16_t y) {
  if (panel.panel == nullptr || panel.panel->kind != PanelKind::Alert) return -1;
  const PanelChrome chrome = panelChrome(font, *panel.panel, panel.bounds);
  if (chrome.body.empty()) return -1;
  const AlertListLayout list = layoutAlertList(font, dashboard, *panel.panel, chrome.body);
  for (size_t i = 0; i < list.rows.size(); ++i) {
    if (list.rows[i].contains(x, y)) return static_cast<int>(i);
  }
  return -1;
}

const Event* alertEventAt(const Dashboard& dashboard, const PanelLayout& panel, int16_t x,
                          int16_t y) {
  if (panel.panel == nullptr || panel.panel->kind != PanelKind::Alert) return nullptr;
  FontManager& font = FontManager::instance();
  const PanelChrome chrome = panelChrome(font, *panel.panel, panel.bounds);
  if (chrome.body.empty()) return nullptr;
  const AlertListLayout list = layoutAlertList(font, dashboard, *panel.panel, chrome.body);
  for (size_t i = 0; i < list.rows.size(); ++i) {
    if (list.rows[i].contains(x, y)) return list.events[i];
  }
  return nullptr;
}

// Event list: severity marker, title (bold while active), time / "cleared" on
// the right; a second caption line with the message when every row has room.
// Row placement comes from layoutAlertList (shared with alertRowAt).
void drawAlertPanel(PortraitCanvas& canvas, FontManager& font, const Dashboard& dashboard,
                    const PanelSpec& panel, const Rect& bounds,
                    const VariableValues& variables) {
  const AlertListLayout list = layoutAlertList(font, dashboard, panel, bounds);
  if (list.events.empty()) {
    if (panel.text.content.empty()) {
      style::emptyState(canvas, font, bounds, "No active alerts");
    } else {
      drawStaticText(canvas, font, panel, bounds, variables);
    }
    return;
  }
  if (list.rows.empty()) return;
  const int16_t caption = style::lineHeight(font);
  const int16_t marker = static_cast<int16_t>(std::max<int>(6, caption / 2));
  const int16_t text_x = static_cast<int16_t>(bounds.x + marker + 8);
  const size_t rows = list.rows.size();
  for (size_t i = 0; i < rows; ++i) {
    const Event& event = *list.events[i];
    const Rect& row = list.rows[i];
    const bool active = event.state == EventState::Active;
    const int16_t text_y = static_cast<int16_t>(row.y + 4);
    style::severityMarker(canvas, row.x, static_cast<int16_t>(text_y + (caption - marker) / 2), marker,
                          event.severity, !active);
    const std::string right = active ? style::formatTime(event.timestamp_ms) : std::string("cleared");
    const int16_t right_w = static_cast<int16_t>(measureText(font, right, kCaption));
    const bool show_right = right_w < bounds.w / 3;
    const int16_t title_w = static_cast<int16_t>(bounds.right() - text_x - (show_right ? right_w + 8 : 0));
    style::text(canvas, font, text_x, text_y, title_w, event.title.empty() ? event.id : event.title,
                kCaption, active);
    if (show_right) style::text(canvas, font, static_cast<int16_t>(bounds.right() - right_w), text_y,
                                right_w, right, kCaption, false, Align::Right);
    if (list.two_lines && !event.message.empty()) {
      style::text(canvas, font, text_x, static_cast<int16_t>(text_y + caption + 2),
                  static_cast<int16_t>(bounds.right() - text_x), event.message, kCaption, false);
    }
    if (i + 1 < rows || list.more)
      style::dottedHLine(canvas, bounds.x, static_cast<int16_t>(bounds.right() - 1),
                         static_cast<int16_t>(row.y + row.h - 2), 2);
  }
  if (list.more) {
    style::text(canvas, font, text_x, static_cast<int16_t>(list.more_bounds.y + 4),
                static_cast<int16_t>(bounds.right() - text_x),
                "+" + std::to_string(list.events.size() - rows) + " more", kCaption);
  }
}

void drawButtonPanel(PortraitCanvas& canvas, FontManager& font, const PanelLayout& layout,
                     const VariableValues& variables) {
  const PanelSpec& panel = *layout.panel;
  for (size_t i = 0; i < panel.actions.size(); ++i) {
    const Action& action = panel.actions[i];
    const Rect bounds = actionBounds(layout, panel.actions.size(), i);
    if (bounds.empty()) continue;
    const bool critical = action.kind == ActionKind::AckAlert;
    const bool tinted = action.kind == ActionKind::SetVariable;
    const int16_t r = static_cast<int16_t>(std::min<int>(8, bounds.h / 4));
    if (critical) {
      canvas.fillRoundRect(bounds.x, bounds.y, bounds.w, bounds.h, r, 0);
    } else {
      if (tinted) canvas.fillRoundRectGray(bounds.x, bounds.y, bounds.w, bounds.h, r, style::kZebraGray);
      canvas.drawRoundRect(bounds.x, bounds.y, bounds.w, bounds.h, r, 0);
      canvas.drawRoundRect(static_cast<int16_t>(bounds.x + 1), static_cast<int16_t>(bounds.y + 1),
                           static_cast<int16_t>(bounds.w - 2), static_cast<int16_t>(bounds.h - 2),
                           static_cast<int16_t>(std::max(0, r - 1)), 0);
    }
    std::string label_text = action.label.empty() ? actionKindName(action.kind) : action.label;
    std::string interpolated;
    std::string error;
    if (interpolateVariables(label_text, variables, interpolated, error)) label_text = interpolated;
    const Rect label_bounds = inset(bounds, 6);
    if (label_bounds.empty()) continue;
    int16_t scale = kBody;
    if (style::lineHeight(font, scale) > label_bounds.h || measureText(font, label_text, scale) > label_bounds.w)
      scale = kCaption;
    const int16_t y = static_cast<int16_t>(label_bounds.y +
                                           std::max(0, (label_bounds.h - style::lineHeight(font, scale)) / 2));
    style::text(canvas, font, label_bounds.x, y, label_bounds.w, label_text, scale, true, Align::Center,
                critical ? 15 : 0);
  }
}

}  // namespace eego::dashboard::ui
