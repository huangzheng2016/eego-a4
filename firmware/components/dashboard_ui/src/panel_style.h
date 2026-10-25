// SPDX-License-Identifier: GPL-3.0-or-later
// Shared visual vocabulary for dashboard panels (e-ink, 1-bit + 4-level gray).
//
// Rules every panel follows:
//  - Card: 1 px rounded frame, kPad inner padding, caption-bold title at the
//    top-left, optional right-aligned header accessory (value or badge).
//  - Essential information is black/white only. Gray (drawPixelGray) is used
//    only for decoration (zebra rows, button tint) because fast refreshes
//    threshold gray (<8 black, >=8 white). Densities (area fill, heatmap) use
//    1-bit ordered patterns so both refresh modes show the same picture.
//  - Text sizes come from the scale ladder: caption 1, body 2, headline 3,
//    display 6. Small panels drop secondary items before shrinking values.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "dashboard_core/model.h"
#include "dashboard_ui/geometry.h"
#include "display_font.h"
#include "font_manager.h"
#include "portrait_canvas.h"

namespace eego::dashboard::ui::style {

inline constexpr int16_t kCaption = 1;
inline constexpr int16_t kBody = 2;
inline constexpr int16_t kHeadline = 3;
inline constexpr int16_t kDisplay = 6;

inline constexpr int16_t kRadius = 6;    // panel frame corner radius
inline constexpr int16_t kPad = 8;       // frame -> content padding
inline constexpr int16_t kTitleGap = 6;  // title row -> body gap
inline constexpr uint8_t kZebraGray = 11;  // light gray, white on fast refresh

enum class Align : uint8_t { Left, Center, Right };

int16_t lineHeight(FontManager& font, int16_t scale = kCaption);
int16_t ascent(FontManager& font, int16_t scale = kCaption);

// ── Numbers ─────────────────────────────────────────────────────────────────

// Display form of a unit: "percent" -> "%", pseudo units ("state") -> "".
std::string displayUnit(std::string_view unit);

struct NumberText {
  std::string number;  // "1,234.5", "1.9M", "-3.2"
  std::string unit;    // display unit, possibly rescaled ("GB")
  bool symbol = false;  // unit is a symbol drawn attached at full size ("%")
  std::string joined() const;  // "86%", "12.4 km/h"
};

// Readable number: thousands separators, decimals capped by magnitude
// (>=1000: 0, >=100: <=1), integral values without decimals, tiny values with
// two significant digits, bytes rescaled to kB/MB/GB. compact uses K/M/B/T.
NumberText formatNumber(double value, std::string_view unit, uint32_t precision,
                        bool compact = false);
std::string formatValueText(double value, std::string_view unit, uint32_t precision,
                            bool compact = false);
// "+1.2", "-3" (no sign for zero).
std::string formatSigned(double value, std::string_view unit, uint32_t precision,
                         bool compact = false);
// "HH:MM" for epoch timestamps; small (non-epoch) values as plain numbers.
std::string formatTime(int64_t timestamp_ms);

// ── Text ────────────────────────────────────────────────────────────────────

// Ellipsized single line aligned inside [x, x+w). Returns drawn width.
int32_t text(PortraitCanvas& canvas, FontManager& font, int16_t x, int16_t y, int16_t w,
             std::string_view value, int16_t scale = kCaption, bool bold = false,
             Align align = Align::Left, uint8_t gray = 0);
// ── Big values ──────────────────────────────────────────────────────────────
//
// Values use "tiers", largest first: the native display fonts (display_font.h,
// largest size = tier 0) and then the UI font at body and caption scale.

int16_t tierCount();
inline constexpr int16_t kLargestTier = 0;
int16_t tierHeight(FontManager& font, int16_t tier);  // line height of the number
int16_t tierAscent(FontManager& font, int16_t tier);  // top -> baseline
int32_t tierTextWidth(FontManager& font, int16_t tier, std::string_view text);
void tierDrawText(PortraitCanvas& canvas, FontManager& font, int16_t tier, int16_t x, int16_t y,
                  std::string_view text, uint8_t ink = 0);

struct ValueFit {
  int16_t tier = -1;  // -1 = nothing fits
  bool unit = true;
  bool compact = false;
  bool small_symbol = false;  // draw "%"/"°C" at unit size instead of value size
  int32_t width = 0;
  bool ok() const { return tier >= 0; }
};
// Largest tier in [max_tier, min_tier] whose value block (prefix + number +
// unit) fits (w, h). Falls back to a small symbol, a compact number, and
// finally dropping the unit, before giving up.
ValueFit fitValue(FontManager& font, double value, std::string_view unit, uint32_t precision,
                  int16_t w, int16_t h, int16_t max_tier = kLargestTier, int16_t min_tier = 99,
                  std::string_view prefix = {});
// Draws prefix + number at fit.tier with the unit baseline-aligned after it.
void drawValue(PortraitCanvas& canvas, FontManager& font, int16_t x, int16_t y,
               double value, std::string_view unit, uint32_t precision, const ValueFit& fit,
               uint8_t ink = 0, std::string_view prefix = {});

// ── Shapes ──────────────────────────────────────────────────────────────────

void fillGray(PortraitCanvas& canvas, const Rect& rect, uint8_t gray);
// 1-bit ordered pattern, level 0 (white) .. 4 (black).
void fillPattern(PortraitCanvas& canvas, const Rect& rect, uint8_t level);
inline bool patternInk(int16_t x, int16_t y, uint8_t level) {
  // 2x2 Bayer: thresholds 0,2,3,1 -> levels 1..3 = 25/50/75 %.
  static constexpr uint8_t kBayer[2][2] = {{0, 2}, {3, 1}};
  if (level == 0) return false;
  if (level >= 4) return true;
  return kBayer[y & 1][x & 1] < level;
}
void dottedHLine(PortraitCanvas& canvas, int16_t x0, int16_t x1, int16_t y, int16_t step = 3);
void dottedVLine(PortraitCanvas& canvas, int16_t x, int16_t y0, int16_t y1, int16_t step = 3);
// Filled triangle pointing up/down, `size` px tall, top-left at (x, y).
void triangle(PortraitCanvas& canvas, int16_t x, int16_t y, int16_t size, bool up);
// Severity marker (critical: filled square, warning: filled triangle,
// info: hollow circle) in a size x size box.
void severityMarker(PortraitCanvas& canvas, int16_t x, int16_t y, int16_t size, Severity severity,
                    bool hollow = false);
// Pill badge right-aligned at `right`; warning outlined, critical inverted.
// Returns width used (0 when it does not fit max_width).
int16_t badge(PortraitCanvas& canvas, FontManager& font, int16_t right, int16_t y,
              int16_t max_width, std::string_view label, Severity severity);
int16_t badgeHeight(FontManager& font);

// Consistent empty / no-data state: a short bar ("—") + caption, centered.
void emptyState(PortraitCanvas& canvas, FontManager& font, const Rect& bounds,
                std::string_view message = "No data");

}  // namespace eego::dashboard::ui::style
