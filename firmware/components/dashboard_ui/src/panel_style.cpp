// SPDX-License-Identifier: GPL-3.0-or-later
#include "panel_style.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <iterator>

#include "dashboard_ui/text.h"

namespace eego::dashboard::ui::style {
namespace {


bool isSymbolUnit(std::string_view unit) {
  return unit == "%" || unit == "\xe2\x80\xb0" /* ‰ */ ||
         (unit.size() >= 2 && unit.substr(0, 2) == "\xc2\xb0" /* ° */);
}

bool isByteUnit(std::string_view unit) {
  return unit == "B" || unit == "byte" || unit == "bytes";
}

// Inserts thousands separators into the integer part of a printf'd number.
std::string groupThousands(const std::string& number) {
  const size_t start = (!number.empty() && (number[0] == '-' || number[0] == '+')) ? 1 : 0;
  size_t end = number.find('.');
  if (end == std::string::npos) end = number.size();
  if (end - start <= 3) return number;
  std::string out = number.substr(0, start);
  const size_t digits = end - start;
  for (size_t i = 0; i < digits; ++i) {
    if (i > 0 && (digits - i) % 3 == 0) out.push_back(',');
    out.push_back(number[start + i]);
  }
  out.append(number, end, std::string::npos);
  return out;
}

std::string trimZeros(std::string number) {
  if (number.find('.') == std::string::npos) return number;
  while (!number.empty() && number.back() == '0') number.pop_back();
  if (!number.empty() && number.back() == '.') number.pop_back();
  return number;
}

std::string printFixed(double value, int decimals) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
  std::string out(buffer);
  // "-0", "-0.0" -> "0", "0.0"
  if (out.size() > 1 && out[0] == '-' && out.find_first_not_of("0.", 1) == std::string::npos)
    out.erase(0, 1);
  return out;
}

std::string compactNumber(double value) {
  static constexpr const char* kSuffix[] = {"", "K", "M", "B", "T"};
  double scaled = value;
  size_t index = 0;
  while (std::fabs(scaled) >= 999.5 && index + 1 < std::size(kSuffix)) {
    scaled /= 1000.0;
    ++index;
  }
  if (std::fabs(scaled) >= 999.5) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1e", value);
    return buffer;
  }
  const double magnitude = std::fabs(scaled);
  const int decimals = index == 0 ? 0 : magnitude < 10 ? 2 : magnitude < 100 ? 1 : 0;
  return trimZeros(printFixed(scaled, decimals)) + kSuffix[index];
}

}  // namespace

int16_t lineHeight(FontManager& font, int16_t scale) {
  return static_cast<int16_t>(font.uiFontHeight() * scale);
}

int16_t ascent(FontManager& font, int16_t scale) {
  return static_cast<int16_t>(font.uiFontAscender() * scale);
}

std::string displayUnit(std::string_view unit) {
  if (unit == "percent" || unit == "pct") return "%";
  if (unit == "state" || unit == "status" || unit == "bool") return {};
  if (unit == "C" || unit == "degC" || unit == "celsius") return "\xc2\xb0" "C";
  if (unit == "F" || unit == "degF" || unit == "fahrenheit") return "\xc2\xb0" "F";
  return sanitizeText(unit, false, 16);
}

std::string NumberText::joined() const {
  if (unit.empty()) return number;
  return symbol ? number + unit : number + " " + unit;
}

NumberText formatNumber(double value, std::string_view unit, uint32_t precision, bool compact) {
  NumberText out;
  out.unit = displayUnit(unit);
  if (!std::isfinite(value)) {
    out.number = "--";
    return out;
  }
  if (isByteUnit(out.unit)) {
    static constexpr const char* kBytes[] = {"B", "kB", "MB", "GB", "TB", "PB"};
    size_t index = 0;
    while (std::fabs(value) >= 1000.0 && index + 1 < std::size(kBytes)) {
      value /= 1000.0;
      ++index;
    }
    out.unit = kBytes[index];
    if (index > 0) precision = std::fabs(value) < 10 ? 1 : 0;
  }
  out.symbol = isSymbolUnit(out.unit);
  const double magnitude = std::fabs(value);
  if ((compact && magnitude >= 10000.0) || magnitude >= 1e15) {
    out.number = compactNumber(value);
    return out;
  }
  int decimals = static_cast<int>(std::min<uint32_t>(precision, 6));
  if (magnitude >= 1000.0) decimals = 0;
  else if (magnitude >= 100.0) decimals = std::min(decimals, 1);
  if (std::fabs(value - std::round(value)) < 1e-9 * std::max(1.0, magnitude)) decimals = 0;
  if (magnitude > 0 && magnitude < 0.5 * std::pow(10.0, -decimals)) {
    // Would print as 0: show two significant digits instead.
    decimals = std::min(8, static_cast<int>(-std::floor(std::log10(magnitude))) + 1);
  }
  out.number = groupThousands(printFixed(value, decimals));
  return out;
}

std::string formatValueText(double value, std::string_view unit, uint32_t precision,
                            bool compact) {
  return formatNumber(value, unit, precision, compact).joined();
}

std::string formatSigned(double value, std::string_view unit, uint32_t precision, bool compact) {
  NumberText text = formatNumber(value, unit, precision, compact);
  if (std::isfinite(value) && value > 0 && text.number.find_first_not_of("0.,") != std::string::npos)
    text.number.insert(text.number.begin(), '+');
  return text.joined();
}

std::string formatTime(int64_t timestamp_ms) {
  // Below ~2001-09 the value is a relative/test counter, not wall time.
  if (timestamp_ms < 1000000000000LL) return std::to_string(timestamp_ms);
  const time_t seconds = static_cast<time_t>(timestamp_ms / 1000);
  struct tm local {};
  if (localtime_r(&seconds, &local) == nullptr) return std::to_string(timestamp_ms);
  char buffer[8];
  std::snprintf(buffer, sizeof(buffer), "%02d:%02d", local.tm_hour, local.tm_min);
  return buffer;
}

int32_t text(PortraitCanvas& canvas, FontManager& font, int16_t x, int16_t y, int16_t w,
             std::string_view value, int16_t scale, bool bold, Align align, uint8_t gray) {
  if (w <= 0 || value.empty()) return 0;
  const std::string fitted = ellipsize(font, value, w, scale);
  if (fitted.empty()) return 0;
  const int32_t width = measureText(font, fitted, scale);
  int32_t left = x;
  if (align == Align::Center) left = x + std::max<int32_t>(0, (w - width) / 2);
  else if (align == Align::Right) left = x + std::max<int32_t>(0, w - width);
  drawTextFit(canvas, font, static_cast<int16_t>(left), y, w, fitted, scale, bold, gray);
  return width;
}

int16_t tierCount() { return static_cast<int16_t>(display_font::count() + 2); }

namespace {

// Tier -> display font size index (largest first), or -1 for the UI font.
int displayIndex(int16_t tier) {
  const int16_t displays = static_cast<int16_t>(display_font::count());
  return tier < displays ? displays - 1 - tier : -1;
}

int16_t uiScale(int16_t tier) {
  return tier == static_cast<int16_t>(display_font::count()) ? kBody : kCaption;
}

// Unit next to a value: about a third of the number height.
int16_t unitScaleFor(FontManager& font, int16_t tier) {
  return tierHeight(font, tier) >= lineHeight(font, kBody) * 2 ? kBody : kCaption;
}

int32_t blockWidth(FontManager& font, int16_t tier, const std::string& number,
                   const NumberText& value, bool with_unit, bool small_symbol) {
  int32_t width = tierTextWidth(font, tier, number);
  if (with_unit && !value.unit.empty()) {
    if (value.symbol && !small_symbol) {
      width += tierTextWidth(font, tier, value.unit);
    } else {
      const int16_t us = unitScaleFor(font, tier);
      width += (value.symbol ? 2 : 2 + 2 * us) + measureText(font, value.unit, us);
    }
  }
  return width;
}

}  // namespace

int16_t tierHeight(FontManager& font, int16_t tier) {
  const int index = displayIndex(tier);
  return index >= 0 ? display_font::lineHeight(static_cast<size_t>(index)) : lineHeight(font, uiScale(tier));
}

int16_t tierAscent(FontManager& font, int16_t tier) {
  const int index = displayIndex(tier);
  return index >= 0 ? display_font::ascent(static_cast<size_t>(index)) : ascent(font, uiScale(tier));
}

int32_t tierTextWidth(FontManager& font, int16_t tier, std::string_view text) {
  const int index = displayIndex(tier);
  if (index < 0) return measureText(font, text, uiScale(tier));
  const std::string clean = sanitizeText(text, false, 64);
  return display_font::textWidth(static_cast<size_t>(index), clean.c_str());
}

void tierDrawText(PortraitCanvas& canvas, FontManager& font, int16_t tier, int16_t x, int16_t y,
                  std::string_view text, uint8_t ink) {
  const int index = displayIndex(tier);
  if (index < 0) {
    const int16_t scale = uiScale(tier);
    drawTextFit(canvas, font, x, y, static_cast<int16_t>(measureText(font, text, scale) + 1), text,
                scale, true, ink);
    return;
  }
  const std::string clean = sanitizeText(text, false, 64);
  display_font::drawText(canvas, static_cast<size_t>(index), x, y, clean.c_str(), ink);
}

ValueFit fitValue(FontManager& font, double value, std::string_view unit, uint32_t precision,
                  int16_t w, int16_t h, int16_t max_tier, int16_t min_tier, std::string_view prefix) {
  const NumberText normal = formatNumber(value, unit, precision, false);
  const NumberText compact = formatNumber(value, unit, precision, true);
  const bool has_compact = compact.number != normal.number;
  const int16_t last = std::min<int16_t>(min_tier, static_cast<int16_t>(tierCount() - 1));
  for (int pass = 0; pass < 2; ++pass) {
    const bool with_unit = pass == 0;
    if (!with_unit && normal.unit.empty()) break;
    for (int16_t tier = std::max<int16_t>(0, max_tier); tier <= last; ++tier) {
      if (tierHeight(font, tier) > h) continue;
      // Full-size symbol, then symbol at unit size, then compact number.
      for (int variant = 0; variant < 4; ++variant) {
        const bool use_compact = variant >= 2;
        const bool small_symbol = (variant & 1) != 0;
        if (use_compact && !has_compact) break;
        if (small_symbol && !(with_unit && normal.symbol)) continue;
        const NumberText& text = use_compact ? compact : normal;
        const int32_t width =
            blockWidth(font, tier, std::string(prefix) + text.number, text, with_unit, small_symbol);
        if (width <= w) return {tier, with_unit, use_compact, small_symbol, width};
      }
    }
  }
  return {};
}

void drawValue(PortraitCanvas& canvas, FontManager& font, int16_t x, int16_t y, double value,
               std::string_view unit, uint32_t precision, const ValueFit& fit, uint8_t ink,
               std::string_view prefix) {
  if (!fit.ok()) return;
  const NumberText number = formatNumber(value, unit, precision, fit.compact);
  const std::string text = std::string(prefix) + number.number;
  const int32_t number_width = tierTextWidth(font, fit.tier, text);
  tierDrawText(canvas, font, fit.tier, x, y, text, ink);
  if (!fit.unit || number.unit.empty()) return;
  if (number.symbol && !fit.small_symbol) {
    tierDrawText(canvas, font, fit.tier, static_cast<int16_t>(x + number_width), y, number.unit, ink);
    return;
  }
  const int16_t us = unitScaleFor(font, fit.tier);
  const int16_t ux = static_cast<int16_t>(x + number_width + (number.symbol ? 2 : 2 + 2 * us));
  const int16_t uy = static_cast<int16_t>(y + tierAscent(font, fit.tier) - ascent(font, us));
  drawTextFit(canvas, font, ux, uy, static_cast<int16_t>(measureText(font, number.unit, us) + 1),
              number.unit, us, false, ink);
}

void fillGray(PortraitCanvas& canvas, const Rect& rect, uint8_t gray) {
  if (gray == 0 || gray >= 15) {
    canvas.fillRect(rect.x, rect.y, rect.w, rect.h, gray == 0 ? 0 : 1);
    return;
  }
  for (int16_t y = rect.y; y < rect.bottom(); ++y)
    for (int16_t x = rect.x; x < rect.right(); ++x) canvas.drawPixelGray(x, y, gray);
}

void fillPattern(PortraitCanvas& canvas, const Rect& rect, uint8_t level) {
  if (rect.empty() || level == 0) return;
  if (level >= 4) {
    canvas.fillRect(rect.x, rect.y, rect.w, rect.h, 0);
    return;
  }
  for (int16_t y = rect.y; y < rect.bottom(); ++y)
    for (int16_t x = rect.x; x < rect.right(); ++x)
      if (patternInk(x, y, level)) canvas.drawPixel(x, y, 0);
}

void dottedHLine(PortraitCanvas& canvas, int16_t x0, int16_t x1, int16_t y, int16_t step) {
  if (step < 1) step = 1;
  for (int16_t x = x0; x <= x1; x = static_cast<int16_t>(x + step)) canvas.drawPixel(x, y, 0);
}

void dottedVLine(PortraitCanvas& canvas, int16_t x, int16_t y0, int16_t y1, int16_t step) {
  if (step < 1) step = 1;
  for (int16_t y = y0; y <= y1; y = static_cast<int16_t>(y + step)) canvas.drawPixel(x, y, 0);
}

void triangle(PortraitCanvas& canvas, int16_t x, int16_t y, int16_t size, bool up) {
  if (size < 2) return;
  const int16_t center = static_cast<int16_t>(x + size / 2);
  for (int16_t row = 0; row < size; ++row) {
    const int16_t half = static_cast<int16_t>((row * size / 2) / (size - 1));
    const int16_t yy = static_cast<int16_t>(up ? y + row : y + size - 1 - row);
    canvas.fillRect(static_cast<int16_t>(center - half), yy, static_cast<int16_t>(half * 2 + 1), 1, 0);
  }
}

void severityMarker(PortraitCanvas& canvas, int16_t x, int16_t y, int16_t size, Severity severity,
                    bool hollow) {
  if (size < 4) return;
  if (hollow || severity == Severity::Info) {
    const int16_t r = static_cast<int16_t>(size / 2 - 1);
    canvas.drawCircle(static_cast<int16_t>(x + size / 2), static_cast<int16_t>(y + size / 2), r, 0);
    if (!hollow) canvas.fillCircle(static_cast<int16_t>(x + size / 2),
                                   static_cast<int16_t>(y + size / 2),
                                   static_cast<int16_t>(std::max(1, r / 3)), 0);
    return;
  }
  if (severity == Severity::Warning) {
    triangle(canvas, x, y, size, true);
    return;
  }
  canvas.fillRect(x, y, size, size, 0);
}

int16_t badgeHeight(FontManager& font) { return static_cast<int16_t>(lineHeight(font) + 2); }

int16_t badge(PortraitCanvas& canvas, FontManager& font, int16_t right, int16_t y,
              int16_t max_width, std::string_view label, Severity severity) {
  if (label.empty() || max_width <= 0) return 0;
  const int16_t pad = 8;
  const int32_t text_width = measureText(font, label, kCaption) + 1;
  const int32_t width = text_width + pad * 2;
  if (width > max_width) return 0;
  const int16_t h = badgeHeight(font);
  const int16_t x = static_cast<int16_t>(right - width);
  const int16_t r = static_cast<int16_t>(h / 2);
  if (severity == Severity::Critical) {
    canvas.fillRoundRect(x, y, static_cast<int16_t>(width), h, r, 0);
    drawTextFit(canvas, font, static_cast<int16_t>(x + pad), static_cast<int16_t>(y + 1),
                static_cast<int16_t>(text_width), label, kCaption, true, 15);
  } else {
    canvas.drawRoundRect(x, y, static_cast<int16_t>(width), h, r, 0);
    if (severity == Severity::Warning)
      canvas.drawRoundRect(static_cast<int16_t>(x + 1), static_cast<int16_t>(y + 1),
                           static_cast<int16_t>(width - 2), static_cast<int16_t>(h - 2),
                           static_cast<int16_t>(r - 1), 0);
    drawTextFit(canvas, font, static_cast<int16_t>(x + pad), static_cast<int16_t>(y + 1),
                static_cast<int16_t>(text_width), label, kCaption, true, 0);
  }
  return static_cast<int16_t>(width);
}

void emptyState(PortraitCanvas& canvas, FontManager& font, const Rect& bounds,
                std::string_view message) {
  if (bounds.empty()) return;
  const int16_t caption = lineHeight(font);
  const int16_t bar_w = static_cast<int16_t>(std::min<int>(28, bounds.w / 2));
  const int16_t bar_h = 3;
  const bool with_text = !message.empty() && bounds.h >= bar_h + 6 + caption;
  const int16_t block = static_cast<int16_t>(bar_h + (with_text ? 6 + caption : 0));
  const int16_t top = static_cast<int16_t>(bounds.y + std::max(0, (bounds.h - block) / 2));
  canvas.fillRect(static_cast<int16_t>(bounds.x + (bounds.w - bar_w) / 2), top, bar_w, bar_h, 0);
  if (with_text) {
    text(canvas, font, bounds.x, static_cast<int16_t>(top + bar_h + 6), bounds.w, message,
         kCaption, false, Align::Center);
  }
}

}  // namespace eego::dashboard::ui::style
