// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "dashboard_ui/text.h"
#include "panel_draw.h"

namespace eego::dashboard::ui {
namespace {

// 字号阶梯（与设备页 panel_render 的 TextStyle 一致）：说明/标签 1x、正文/按钮 2x、
// 次级数值 3x、主数值最大 6x。自动选字号时只在此阶梯内取最大可放下者。
constexpr int16_t kScaleCaption = 1;
constexpr int16_t kScaleBody = 2;
constexpr int16_t kScaleHeadline = 3;
constexpr int16_t kScaleDisplay = 6;

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

std::string formatValue(double value, std::string_view unit, uint32_t precision) {
  if (!std::isfinite(value)) return "--";
  char number[80];
  precision = std::min<uint32_t>(precision, 8);
  const double magnitude = std::fabs(value);
  if ((magnitude != 0 && magnitude < 0.0001) || magnitude >= 1000000000.0) {
    std::snprintf(number, sizeof(number), "%.*g", static_cast<int>(precision + 1), value);
  } else {
    std::snprintf(number, sizeof(number), "%.*f", static_cast<int>(precision), value);
  }
  std::string out(number);
  if (!unit.empty()) {
    if (unit != "%") out.push_back(' ');
    out += sanitizeText(unit, false, 32);
  }
  return out;
}

std::string_view label(const MetricSample& sample, std::string_view key) {
  for (const KeyValue& item : sample.labels) {
    if (item.key == key) return item.value;
  }
  return {};
}

Severity severityFor(const PanelSpec& panel, double value) {
  Severity severity = Severity::Info;
  for (const Threshold& threshold : panel.thresholds) {
    const bool crossed = panel.style.invert ? value <= threshold.value : value >= threshold.value;
    if (crossed && static_cast<uint8_t>(threshold.severity) >= static_cast<uint8_t>(severity))
      severity = threshold.severity;
  }
  return severity;
}

uint8_t severityFill(Severity severity) {
  if (severity == Severity::Critical) return 0;
  if (severity == Severity::Warning) return 5;
  return 11;
}

uint8_t severityText(Severity severity) {
  return severity == Severity::Info ? 0 : 15;
}

void centered(PortraitCanvas& canvas, FontManager& font, const Rect& bounds,
              std::string_view text, int16_t scale, bool bold = false, uint8_t gray = 0) {
  const std::string fitted = ellipsize(font, text, bounds.w, scale);
  const int32_t width = measureText(font, fitted, scale);
  const int16_t x = bounds.x + std::max<int32_t>(0, (bounds.w - width) / 2);
  const int16_t y = bounds.y + std::max(0, (bounds.h - font.uiFontHeight() * scale) / 2);
  drawTextFit(canvas, font, x, y, bounds.w, fitted, scale, bold, gray);
}

void emptyState(PortraitCanvas& canvas, FontManager& font, const Rect& bounds,
                std::string_view message = "No data") {
  centered(canvas, font, bounds, message, kScaleCaption, false, 6);
}

std::string interpolatedText(const PanelSpec& panel, const VariableValues& variables) {
  std::string output;
  std::string error;
  if (!interpolateVariables(panel.text.content, variables, output, error)) return "Invalid text";
  return sanitizeText(output, true);
}

void drawStaticText(PortraitCanvas& canvas, FontManager& font, const PanelSpec& panel,
                    const Rect& bounds, const VariableValues& variables) {
  const std::string content = interpolatedText(panel, variables);
  if (content.empty()) {
    emptyState(canvas, font, bounds);
    return;
  }
  const int16_t line_height = font.uiFontHeight() + 4;
  drawTextWrapped(canvas, font, bounds, content, kScaleCaption, line_height,
                  std::min<size_t>(panel.text.max_lines, bounds.h / line_height), 0);
}

void drawDefaultCard(PortraitCanvas& canvas, FontManager& font, const PanelSpec& panel,
                     const std::vector<const MetricSample*>& samples, const Rect& bounds) {
  const MetricSample& latest = *samples.back();
  const Severity severity = severityFor(panel, latest.value);
  const std::string value = formatValue(latest.value,
                                        panel.style.unit.empty() ? std::string_view(latest.unit) : std::string_view(panel.style.unit),
                                        panel.style.precision);
  const int16_t font_height = font.uiFontHeight();
  if (font_height <= 0 || bounds.h < font_height) return;
  bool show_delta = samples.size() > 1 && bounds.h >= font_height * 2 + 6;
  Rect value_bounds = bounds;
  if (show_delta) value_bounds.h -= font_height + 6;
  if (value_bounds.h < font_height) {
    show_delta = false;
    value_bounds = bounds;
  }
  int16_t scale = std::min<int16_t>(kScaleDisplay,
                                    std::max<int16_t>(kScaleCaption, value_bounds.h / font_height));
  while (scale > kScaleCaption &&
         (measureText(font, value, scale) > value_bounds.w || font_height * scale > value_bounds.h)) {
    --scale;
  }
  centered(canvas, font, value_bounds, value, scale, true,
           severity == Severity::Info ? 0 : severityFill(severity));
  if (show_delta) {
    const MetricSample& previous = *samples[samples.size() - 2];
    const double delta = latest.value - previous.value;
    const std::string delta_text = std::string(delta >= 0 ? "+" : "") +
                                   formatValue(delta, panel.style.unit.empty() ? std::string_view(latest.unit) : std::string_view(panel.style.unit),
                                               panel.style.precision);
    drawTextFit(canvas, font, bounds.x, bounds.y + bounds.h - font_height, bounds.w,
                delta_text, kScaleCaption, false, 6);
  }
}

int16_t scaleForText(FontManager& font, std::string_view text, const Rect& bounds,
                     int16_t max_scale) {
  const int16_t font_height = font.uiFontHeight();
  if (max_scale < 1 || font_height <= 0 || bounds.w <= 0 || bounds.h < font_height) return 0;
  int16_t scale = std::min(max_scale, static_cast<int16_t>(bounds.h / font_height));
  while (scale > 1 && measureText(font, text, scale) > bounds.w) --scale;
  return scale;
}

void drawDeltaCard(PortraitCanvas& canvas, FontManager& font, const PanelSpec& panel,
                   const std::vector<const MetricSample*>& samples, const Rect& bounds) {
  const MetricSample& latest = *samples.back();
  const double previous = samples.size() > 1 ? samples[samples.size() - 2]->value : latest.value;
  const double delta = latest.value - previous;
  const std::string current = formatValue(latest.value,
                                          panel.style.unit.empty() ? std::string_view(latest.unit) : std::string_view(panel.style.unit),
                                          panel.style.precision);
  const std::string change = std::string(delta >= 0 ? "+" : "") +
                             formatValue(delta, panel.style.unit.empty() ? std::string_view(latest.unit) : std::string_view(panel.style.unit),
                                         panel.style.precision);
  // 当前值：卡片够高（≥ 两行正文）时用正文字号，否则说明字号；变化量占余下高度。
  const int16_t font_height = font.uiFontHeight();
  const int16_t current_line =
      bounds.h >= (font_height * kScaleBody + 4) * 2 ? font_height * kScaleBody + 4
                                                     : font_height + 4;
  Rect current_bounds = bounds;
  current_bounds.h = std::min(current_line, bounds.h);
  const int16_t current_scale = scaleForText(font, current, current_bounds, kScaleBody);
  if (current_scale > 0) {
    drawTextFit(canvas, font, current_bounds.x, current_bounds.y, current_bounds.w, current,
                current_scale, true, 0);
  }
  Rect change_bounds{bounds.x, static_cast<int16_t>(bounds.y + current_bounds.h), bounds.w,
                     static_cast<int16_t>(bounds.h - current_bounds.h)};
  const int16_t change_scale = scaleForText(font, change, change_bounds, kScaleHeadline);
  if (change_scale > 0) {
    centered(canvas, font, change_bounds, change, change_scale, true, delta < 0 ? 6 : 0);
  }
}

void drawProgressCard(PortraitCanvas& canvas, FontManager& font, const PanelSpec& panel,
                      const MetricSample& sample, const Rect& bounds) {
  const std::string value = formatValue(sample.value,
                                        panel.style.unit.empty() ? "%" : std::string_view(panel.style.unit),
                                        panel.style.precision);
  Rect value_bounds = bounds;
  value_bounds.h = std::max<int16_t>(0, bounds.h - 18);
  const int16_t value_scale = scaleForText(font, value, value_bounds, kScaleHeadline);
  if (value_scale > 0) centered(canvas, font, value_bounds, value, value_scale, true, 0);
  const int16_t bar_y = bounds.y + bounds.h - 12;
  canvas.drawRect(bounds.x, bar_y, bounds.w, 10, 0);
  const double clamped = std::isfinite(sample.value) ? std::clamp(sample.value, 0.0, 100.0) : 0.0;
  const int16_t filled = static_cast<int16_t>((bounds.w - 4) * clamped / 100.0);
  if (filled > 0) {
    const Severity severity = severityFor(panel, sample.value);
    for (int16_t x = 0; x < filled; ++x) {
      for (int16_t y = 0; y < 6; ++y)
        canvas.drawPixelGray(bounds.x + 2 + x, bar_y + 2 + y, severityFill(severity));
    }
  }
}

void drawStatusCard(PortraitCanvas& canvas, FontManager& font, const PanelSpec& panel,
                    const MetricSample& sample, const Rect& bounds) {
  const Severity severity = severityFor(panel, sample.value);
  std::string status(label(sample, "status"));
  if (status.empty()) status = label(sample, "state");
  if (status.empty()) {
    for (const Threshold& threshold : panel.thresholds) {
      const bool crossed = panel.style.invert ? sample.value <= threshold.value : sample.value >= threshold.value;
      if (crossed && threshold.severity == severity && !threshold.label.empty()) status = threshold.label;
    }
  }
  if (status.empty()) status = severityName(severity);
  const Rect chip = inset(bounds, 4);
  for (int16_t y = chip.y; y < chip.bottom(); ++y) {
    for (int16_t x = chip.x; x < chip.right(); ++x) canvas.drawPixelGray(x, y, severityFill(severity));
  }
  centered(canvas, font, chip, status, chip.h >= 48 ? 2 : 1, true, severityText(severity));
}

struct PlotPoint {
  int16_t x;
  int16_t y;
};

void drawChartSeries(PortraitCanvas& canvas, FontManager& font, const PanelSpec& panel,
                     const std::vector<const MetricSample*>& samples, const Rect& bounds) {
  Rect plot = bounds;
  const bool legend = panel.style.show_legend && panel.chart.kind != ChartKind::Sparkline &&
                      bounds.h >= font.uiFontHeight() + 24;
  if (legend) plot.h -= font.uiFontHeight() + 3;
  if (plot.w < 2 || plot.h < 2 || samples.empty()) {
    emptyState(canvas, font, bounds);
    return;
  }
  const MetricSample* first = firstFiniteSample(samples);
  if (first == nullptr) {
    emptyState(canvas, font, bounds);
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
    emptyState(canvas, font, bounds);
    return;
  }
  if (panel.chart.kind == ChartKind::Bar) {
    if (!panel.chart.has_y_min) minimum = std::min(0.0, minimum);
    if (!panel.chart.has_y_max) maximum = std::max(0.0, maximum);
  }
  expandConstantRange(minimum, maximum);
  const auto y_for = [&](double value) {
    double ratio = 0.5;
    if (!normalizedRatio(value, minimum, maximum, ratio)) ratio = 0.5;
    if (panel.style.invert) ratio = 1.0 - ratio;
    return static_cast<int16_t>(plot.y + plot.h - 1 - ratio * (plot.h - 1));
  };

  if (panel.chart.kind == ChartKind::Bar) {
    const int16_t zero_y = y_for(0);
    const int16_t slot = std::max(1, plot.w / static_cast<int16_t>(samples.size()));
    const int16_t bar_width = std::max(1, slot - 2);
    for (size_t i = 0; i < samples.size(); ++i) {
      if (!std::isfinite(samples[i]->value)) continue;
      const int16_t x = plot.x + i * plot.w / samples.size() + (slot - bar_width) / 2;
      const int16_t y = y_for(samples[i]->value);
      canvas.fillRect(x, std::min(y, zero_y), bar_width, std::max(1, std::abs(zero_y - y) + 1), 0);
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
      const int16_t baseline = y_for(0);
      for (size_t i = 1; i < points.size(); ++i) {
        const int16_t x0 = points[i - 1].x;
        const int16_t x1 = points[i].x;
        for (int32_t x = x0; x <= x1; ++x) {
          const int32_t span = std::max(1, x1 - x0);
          const int16_t y = points[i - 1].y + (points[i].y - points[i - 1].y) * (x - x0) / span;
          for (int16_t yy = std::min(y, baseline); yy <= std::max(y, baseline); ++yy)
            canvas.drawPixelGray(x, yy, 11);
        }
      }
    }
    const int16_t line_width = std::min<int16_t>(panel.style.line_width, 8);
    for (size_t i = 1; i < points.size(); ++i) {
      for (int16_t offset = 0; offset < line_width; ++offset) {
        canvas.drawLine(points[i - 1].x, points[i - 1].y + offset - line_width / 2,
                        points[i].x, points[i].y + offset - line_width / 2, 0);
      }
    }
    if (points.size() == 1 || panel.chart.show_points) {
      for (const PlotPoint& point : points) canvas.fillCircle(point.x, point.y, 2, 0);
    }
  }

  if (legend) {
    const int16_t y = plot.y + plot.h + 3;
    drawTextFit(canvas, font, plot.x, y, plot.w / 2,
                formatValue(maximum, panel.style.unit, panel.style.precision), kScaleCaption,
                false, 6);
    const std::string low = formatValue(minimum, panel.style.unit, panel.style.precision);
    const int16_t width = measureText(font, low, 1);
    drawTextFit(canvas, font, plot.x + plot.w - std::min<int16_t>(plot.w / 2, width), y,
                plot.w / 2, low, kScaleCaption, false, 6);
  }
}

std::string tableValue(const MetricSample& sample, const TableColumn& column,
                       const PanelSpec& panel) {
  if (column.key == "value") {
    const std::string_view unit = !column.unit.empty() ? std::string_view(column.unit) :
                                  (!panel.style.unit.empty() ? std::string_view(panel.style.unit) : std::string_view(sample.unit));
    return formatValue(sample.value, unit, panel.style.precision);
  }
  if (column.key == "metric") return sample.metric;
  if (column.key == "source" || column.key == "source_id") return sample.source_id;
  if (column.key == "time" || column.key == "timestamp" || column.key == "timestamp_ms")
    return std::to_string(sample.timestamp_ms);
  return std::string(label(sample, column.key));
}

void drawDefaultTable(PortraitCanvas& canvas, FontManager& font, const PanelSpec& panel,
                      const std::vector<const MetricSample*>& samples, const Rect& bounds) {
  if (samples.empty()) {
    emptyState(canvas, font, bounds);
    return;
  }
  const int16_t row_height = font.uiFontHeight() + 6;
  const bool header = panel.style.show_legend && bounds.h >= row_height * 2;
  const size_t rows = std::min<size_t>(samples.size(), (bounds.h - (header ? row_height : 0)) / row_height);
  if (rows == 0) return;
  std::vector<TableColumn> columns = panel.columns;
  if (columns.empty()) columns.push_back({"value", "Value", "", 1});
  uint32_t weight = 0;
  for (const TableColumn& column : columns) weight += std::max<uint32_t>(1, column.width);
  int16_t y = bounds.y;
  for (size_t row = 0; row < rows + (header ? 1 : 0); ++row) {
    const bool head = header && row == 0;
    const size_t data_row = row - (header ? 1 : 0);
    const MetricSample* sample = head ? nullptr : samples[samples.size() - 1 - data_row];
    int32_t x = bounds.x;
    for (size_t col = 0; col < columns.size(); ++col) {
      const int32_t remaining = bounds.x + bounds.w - x;
      const int16_t width = col + 1 == columns.size() ? remaining :
                            static_cast<int16_t>(bounds.w * columns[col].width / weight);
      const std::string value = head ? (columns[col].title.empty() ? columns[col].key : columns[col].title) :
                                tableValue(*sample, columns[col], panel);
      drawTextFit(canvas, font, x + 2, y, width - 4, value, 1, head, head ? 0 : 6);
      x += width;
    }
    y += row_height;
    canvas.drawLine(bounds.x, y - 2, bounds.x + bounds.w - 1, y - 2, 0);
  }
}

void drawStatusGrid(PortraitCanvas& canvas, FontManager& font, const PanelSpec& panel,
                    const std::vector<const MetricSample*>& samples, const Rect& bounds) {
  if (samples.empty()) {
    emptyState(canvas, font, bounds);
    return;
  }
  const size_t count = std::min<size_t>(samples.size(), 32);
  const size_t columns = bounds.w >= 240 ? 4 : 2;
  const size_t rows = (count + columns - 1) / columns;
  const int16_t cell_w = bounds.w / columns;
  const int16_t cell_h = bounds.h / rows;
  for (size_t i = 0; i < count; ++i) {
    const MetricSample& sample = *samples[samples.size() - count + i];
    const Rect cell{static_cast<int16_t>(bounds.x + (i % columns) * cell_w),
                    static_cast<int16_t>(bounds.y + (i / columns) * cell_h), cell_w, cell_h};
    const Severity severity = severityFor(panel, sample.value);
    for (int16_t y = cell.y + 1; y < cell.bottom() - 1; ++y)
      for (int16_t x = cell.x + 1; x < cell.right() - 1; ++x)
        canvas.drawPixelGray(x, y, severityFill(severity));
    canvas.drawRect(cell.x, cell.y, cell.w, cell.h, 0);
    std::string name(label(sample, "name"));
    if (name.empty() && !sample.labels.empty()) name = sample.labels.front().value;
    if (name.empty()) name = sample.metric;
    const Rect text_bounds = inset(cell, 3);
    if (text_bounds.empty()) continue;
    const int16_t font_height = font.uiFontHeight();
    Rect name_bounds = text_bounds;
    const bool has_value = text_bounds.h >= font_height * 2 + 2;
    if (has_value) name_bounds.h -= font_height + 2;
    const int16_t name_scale = scaleForText(font, name, name_bounds, kScaleCaption);
    if (name_scale > 0) {
      drawTextFit(canvas, font, name_bounds.x, name_bounds.y, name_bounds.w, name,
                  name_scale, false, severityText(severity));
    }
    if (has_value) {
      // 值：名称下方剩余高度内取阶梯上最大可放下字号（≤ 正文），贴底绘制
      const int16_t value_h = static_cast<int16_t>(text_bounds.h - font_height - 2);
      const Rect value_bounds{text_bounds.x,
                              static_cast<int16_t>(text_bounds.bottom() - value_h),
                              text_bounds.w, value_h};
      const std::string value = formatValue(sample.value, sample.unit, panel.style.precision);
      const int16_t value_scale = scaleForText(font, value, value_bounds, kScaleBody);
      if (value_scale > 0) {
        drawTextFit(canvas, font, value_bounds.x,
                    static_cast<int16_t>(text_bounds.bottom() - font_height * value_scale),
                    value_bounds.w, value, value_scale, true, severityText(severity));
      }
    }
  }
}

void drawHeatmap(PortraitCanvas& canvas, FontManager& font, const PanelSpec&,
                 const std::vector<const MetricSample*>& samples, const Rect& bounds) {
  if (samples.empty()) {
    emptyState(canvas, font, bounds);
    return;
  }
  const size_t count = std::min<size_t>(samples.size(), 64);
  const MetricSample* first = firstFiniteSample(samples);
  if (first == nullptr) {
    emptyState(canvas, font, bounds);
    return;
  }
  double minimum = first->value;
  double maximum = first->value;
  for (const MetricSample* sample : samples) {
    if (!std::isfinite(sample->value)) continue;
    minimum = std::min(minimum, sample->value);
    maximum = std::max(maximum, sample->value);
  }
  expandConstantRange(minimum, maximum);
  const size_t columns = count <= 8 ? count : 8;
  const size_t rows = (count + columns - 1) / columns;
  const int16_t cell_w = bounds.w / columns;
  const int16_t cell_h = bounds.h / rows;
  for (size_t i = 0; i < count; ++i) {
    const MetricSample& sample = *samples[samples.size() - count + i];
    double ratio = 0;
    if (!normalizedRatio(sample.value, minimum, maximum, ratio)) continue;
    const uint8_t gray = static_cast<uint8_t>(15 - static_cast<int>(ratio * 15));
    const Rect cell{static_cast<int16_t>(bounds.x + (i % columns) * cell_w),
                    static_cast<int16_t>(bounds.y + (i / columns) * cell_h), cell_w, cell_h};
    for (int16_t y = cell.y; y < cell.bottom(); ++y)
      for (int16_t x = cell.x; x < cell.right(); ++x) canvas.drawPixelGray(x, y, gray);
    canvas.drawRect(cell.x, cell.y, cell.w, cell.h, gray > 7 ? 0 : 1);
  }
}

void drawTimeline(PortraitCanvas& canvas, FontManager& font, const PanelSpec& panel,
                  const std::vector<const MetricSample*>& samples, const Rect& bounds) {
  if (samples.empty()) {
    emptyState(canvas, font, bounds);
    return;
  }
  const size_t count = std::min<size_t>(samples.size(), 16);
  const int16_t y = bounds.y + bounds.h / 2;
  canvas.drawLine(bounds.x, y, bounds.x + bounds.w - 1, y, 0);
  for (size_t i = 0; i < count; ++i) {
    const MetricSample& sample = *samples[samples.size() - count + i];
    const int16_t x = bounds.x + (count == 1 ? bounds.w / 2 : i * (bounds.w - 1) / (count - 1));
    const Severity severity = severityFor(panel, sample.value);
    canvas.fillCircle(x, y, 3, 0);
    if (sample.timestamp_ms != 0 && bounds.h >= 36 && (i == 0 || i + 1 == count)) {
      drawTextFit(canvas, font, std::max<int>(bounds.x, x - 32), i == 0 ? y + 8 : y - 18,
                  64, std::to_string(sample.timestamp_ms), kScaleCaption, false, 6);
    }
    if (severity != Severity::Info) canvas.drawCircle(x, y, 5, 0);
  }
}

const PanelSpec* findDashboardPanel(const Dashboard& dashboard, std::string_view id) {
  for (const Page& page : dashboard.pages) {
    for (const PanelSpec& panel : page.panels) {
      if (panel.id == id) return &panel;
    }
  }
  return nullptr;
}

void drawLinkedCards(PortraitCanvas& canvas, FontManager& font, const Dashboard& dashboard,
                     const PanelSpec& panel, const Rect& bounds,
                     const VariableValues& variables) {
  const size_t count = panel.style.variant == "dual" ? 2 : 3;
  std::vector<const PanelSpec*> cards{&panel};
  for (const std::string& id : panel.linked_panel_ids) {
    const PanelSpec* linked = findDashboardPanel(dashboard, id);
    if (linked == nullptr || linked->kind != PanelKind::Card) {
      emptyState(canvas, font, bounds, "Invalid links");
      return;
    }
    cards.push_back(linked);
  }
  if (cards.size() != count) {
    emptyState(canvas, font, bounds, "Invalid links");
    return;
  }

  const size_t columns = bounds.w >= static_cast<int16_t>(count * 96) ? count : 1;
  const size_t rows = (count + columns - 1) / columns;
  const int16_t gap = 6;
  const int16_t cell_w = (bounds.w - static_cast<int16_t>(columns - 1) * gap) /
                         static_cast<int16_t>(columns);
  const int16_t cell_h = (bounds.h - static_cast<int16_t>(rows - 1) * gap) /
                         static_cast<int16_t>(rows);
  if (cell_w <= 0 || cell_h <= 0) return;
  for (size_t i = 0; i < count; ++i) {
    const PanelSpec& card = *cards[i];
    Rect cell{bounds.x, bounds.y, cell_w, cell_h};
    cell.x = static_cast<int16_t>(bounds.x + (i % columns) * (cell_w + gap));
    cell.y = static_cast<int16_t>(bounds.y + (i / columns) * (cell_h + gap));
    canvas.drawRect(cell.x, cell.y, cell.w, cell.h, 0);
    Rect label_bounds = inset(cell, 3);
    label_bounds.h = std::min(font.uiFontHeight(), label_bounds.h);
    std::string label = card.title.empty() ? card.metric : card.title;
    std::string interpolated;
    std::string error;
    if (!card.title.empty() && interpolateVariables(card.title, variables, interpolated, error))
      label = interpolated;
    const int16_t label_scale = scaleForText(font, label, label_bounds, kScaleCaption);
    if (label_scale > 0) {
      drawTextFit(canvas, font, label_bounds.x, label_bounds.y, label_bounds.w, label,
                  label_scale, false, 6);
    }
    Rect value = inset(cell, 3);
    if (label_scale > 0) {
      const int16_t reserved = std::min<int16_t>(label_bounds.h + 4, value.h);
      value.y += reserved;
      value.h -= reserved;
    }
    if (value.h < font.uiFontHeight()) continue;
    const std::vector<const MetricSample*> samples = series(dashboard, card);
    if (samples.empty()) {
      emptyState(canvas, font, value);
    } else {
      drawDefaultCard(canvas, font, card, samples, value);
    }
  }
}

}  // namespace

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
    drawDefaultCard(canvas, font, panel, samples, bounds);
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

void drawAlertPanel(PortraitCanvas& canvas, FontManager& font, const Dashboard& dashboard,
                    const PanelSpec& panel, const Rect& bounds,
                    const VariableValues& variables) {
  std::vector<const Event*> events;
  for (const Event& event : dashboard.events) {
    if (events.size() >= 256) break;
    if (!alertEventMatches(dashboard, panel, event)) continue;
    events.push_back(&event);
  }
  std::stable_sort(events.begin(), events.end(), [](const Event* lhs, const Event* rhs) {
    if (lhs->state != rhs->state) return lhs->state == EventState::Active;
    return lhs->timestamp_ms > rhs->timestamp_ms;
  });
  if (events.empty()) {
    if (panel.text.content.empty()) {
      emptyState(canvas, font, bounds, "No alerts");
    } else {
      drawStaticText(canvas, font, panel, bounds, variables);
    }
    return;
  }
  const int16_t row_height = std::max<int16_t>(font.uiFontHeight() + 7, 14);
  const size_t rows = std::min<size_t>(events.size(), bounds.h / row_height);
  for (size_t i = 0; i < rows; ++i) {
    const Event& event = *events[i];
    const int16_t y = bounds.y + i * row_height;
    const Severity severity = event.state == EventState::Cleared ? Severity::Info : event.severity;
    const uint8_t fill = severityFill(severity);
    for (int16_t yy = y + 1; yy < y + row_height - 2; ++yy)
      for (int16_t xx = bounds.x + 1; xx < bounds.x + 8; ++xx) canvas.drawPixelGray(xx, yy, fill);
    drawTextFit(canvas, font, bounds.x + 12, y, bounds.w - 74, event.title, 1,
                event.state == EventState::Active, 0);
    drawTextFit(canvas, font, bounds.x + bounds.w - 58, y, 56,
                event.state == EventState::Active ? severityName(event.severity) : "cleared",
                1, false, 6);
    canvas.drawLine(bounds.x, y + row_height - 2, bounds.x + bounds.w - 1,
                    y + row_height - 2, 0);
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
    if (critical || tinted) {
      const uint8_t gray = critical ? 0 : 11;
      for (int16_t y = bounds.y; y < bounds.bottom(); ++y)
        for (int16_t x = bounds.x; x < bounds.right(); ++x) canvas.drawPixelGray(x, y, gray);
    }
    canvas.drawRect(bounds.x, bounds.y, bounds.w, bounds.h, 0);
    std::string label_text = action.label.empty() ? actionKindName(action.kind) : action.label;
    std::string interpolated;
    std::string error;
    if (interpolateVariables(label_text, variables, interpolated, error)) label_text = interpolated;
    const Rect label_bounds = inset(bounds, 2);
    const int16_t label_scale = scaleForText(font, label_text, label_bounds, kScaleBody);
    if (label_scale > 0) {
      centered(canvas, font, label_bounds, label_text, label_scale, action.confirm,
                 critical ? 15 : 0);
    }
  }
}

}  // namespace eego::dashboard::ui
