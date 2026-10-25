// SPDX-License-Identifier: GPL-3.0-or-later
#include "dashboard_ui/text.h"

#include <algorithm>
#include <vector>

#include "panel_style.h"

namespace eego::dashboard::ui {
namespace {

// 字号阶梯（与 panels.cpp / 设备页 TextStyle 一致）：说明 1x、正文 2x。
constexpr int16_t kScaleCaption = 1;
constexpr int16_t kScaleBody = 2;

size_t validCharLength(std::string_view text, size_t offset) {
  const unsigned char lead = static_cast<unsigned char>(text[offset]);
  if (lead < 0x80) return 1;
  size_t length = 0;
  uint32_t codepoint = 0;
  if (lead >= 0xc2 && lead <= 0xdf) {
    length = 2;
    codepoint = lead & 0x1f;
  } else if (lead >= 0xe0 && lead <= 0xef) {
    length = 3;
    codepoint = lead & 0x0f;
  } else if (lead >= 0xf0 && lead <= 0xf4) {
    length = 4;
    codepoint = lead & 0x07;
  } else {
    return 0;
  }
  if (offset + length > text.size()) return 0;
  for (size_t i = 1; i < length; ++i) {
    const unsigned char continuation = static_cast<unsigned char>(text[offset + i]);
    if ((continuation & 0xc0) != 0x80) return 0;
    codepoint = (codepoint << 6) | (continuation & 0x3f);
  }
  if ((length == 3 && codepoint < 0x800) || (length == 4 && codepoint < 0x10000) ||
      (codepoint >= 0xd800 && codepoint <= 0xdfff) || codepoint > 0x10ffff) {
    return 0;
  }
  return length;
}

size_t codepointCount(std::string_view text, size_t begin, size_t end) {
  size_t count = 0;
  size_t offset = begin;
  while (offset < end) {
    size_t length = validCharLength(text, offset);
    if (length == 0 || offset + length > end) length = 1;
    offset += length;
    ++count;
  }
  return count;
}

size_t offsetForCodepoint(std::string_view text, size_t begin, size_t count, size_t end) {
  size_t offset = begin;
  for (size_t i = 0; i < count && offset < end; ++i) {
    size_t length = validCharLength(text, offset);
    if (length == 0 || offset + length > end) length = 1;
    offset += length;
  }
  return offset;
}

std::string ellipsizeValid(FontManager& font, std::string_view text, int16_t max_width,
                           int16_t scale) {
  if (max_width <= 0) return {};
  if (measureText(font, text, scale) <= max_width) return std::string(text);
  if (measureText(font, "...", scale) > max_width) return {};
  const size_t count = codepointCount(text, 0, text.size());
  size_t low = 0;
  size_t high = count;
  while (low < high) {
    const size_t middle = low + (high - low + 1) / 2;
    const size_t offset = offsetForCodepoint(text, 0, middle, text.size());
    const std::string candidate = std::string(text.substr(0, offset)) + "...";
    if (measureText(font, candidate, scale) <= max_width) {
      low = middle;
    } else {
      high = middle - 1;
    }
  }
  return std::string(text.substr(0, offsetForCodepoint(text, 0, low, text.size()))) + "...";
}

size_t longestFit(FontManager& font, std::string_view text, size_t begin, size_t end,
                  int16_t width, int16_t scale) {
  const size_t count = codepointCount(text, begin, end);
  size_t low = 0;
  size_t high = count;
  while (low < high) {
    const size_t middle = low + (high - low + 1) / 2;
    const size_t offset = offsetForCodepoint(text, begin, middle, end);
    if (measureText(font, text.substr(begin, offset - begin), scale) <= width) {
      low = middle;
    } else {
      high = middle - 1;
    }
  }
  return offsetForCodepoint(text, begin, low, end);
}

std::vector<std::string> wrapValid(FontManager& font, std::string_view text, int16_t width,
                                   int16_t scale, size_t max_lines, bool& truncated) {
  std::vector<std::string> lines;
  truncated = false;
  if (width <= 0 || max_lines == 0) {
    truncated = !text.empty();
    return lines;
  }
  max_lines = std::min<size_t>(max_lines, 64);
  size_t position = 0;
  bool complete = false;
  while (!complete && lines.size() < max_lines) {
    const size_t newline = text.find('\n', position);
    const bool last_paragraph = newline == std::string_view::npos;
    const size_t end = last_paragraph ? text.size() : newline;
    if (position == end) {
      lines.emplace_back();
    } else {
      size_t cursor = position;
      while (cursor < end && lines.size() < max_lines) {
        while (cursor < end && text[cursor] == ' ') ++cursor;
        if (cursor == end) break;
        size_t fit = longestFit(font, text, cursor, end, width, scale);
        if (fit <= cursor) {
          const size_t length = validCharLength(text, cursor);
          fit = cursor + (length == 0 ? 1 : length);
        }
        size_t line_end = fit;
        size_t next = fit;
        if (fit < end) {
          const size_t space = text.rfind(' ', fit - 1);
          if (space != std::string_view::npos && space >= cursor && space > cursor) {
            line_end = space;
            next = space + 1;
          }
        }
        lines.emplace_back(text.substr(cursor, line_end - cursor));
        cursor = next;
      }
      while (cursor < end && text[cursor] == ' ') ++cursor;
      if (cursor < end) break;  // paragraph cut off by max_lines: truncated
      complete = last_paragraph;
    }
    if (last_paragraph) {
      if (position == end) complete = true;
    } else {
      position = newline + 1;
      if (position == text.size()) complete = true;
    }
  }
  truncated = !complete;
  if (truncated && !lines.empty()) lines.back() = ellipsizeValid(font, lines.back() + "...", width, scale);
  return lines;
}

void drawRaw(PortraitCanvas& canvas, FontManager& font, int16_t x, int16_t y,
             const std::string& text, int16_t scale, bool bold, uint8_t gray) {
  if (text.empty()) return;
  if (gray == 0) {
    font.drawText(canvas, x, y, text.c_str(), scale, bold);
  } else {
    font.drawTextGray(canvas, x, y, text.c_str(), scale, gray);
    if (bold) font.drawTextGray(canvas, x + 1, y, text.c_str(), scale, gray);
  }
}

void drawMarkdownInline(PortraitCanvas& canvas, FontManager& font, int16_t x, int16_t y,
                        int16_t width, std::string_view line, int16_t scale) {
  size_t position = 0;
  bool bold = false;
  int32_t cursor = x;
  while (position < line.size() && cursor < x + width) {
    const size_t marker = line.find("**", position);
    const size_t end = marker == std::string_view::npos ? line.size() : marker;
    const std::string_view segment = line.substr(position, end - position);
    if (!segment.empty()) {
      const std::string fitted = ellipsizeValid(font, segment, x + width - cursor, scale);
      drawRaw(canvas, font, cursor, y, fitted, scale, bold, 0);
      cursor += measureText(font, fitted, scale);
      if (fitted.size() != segment.size()) return;
    }
    if (marker == std::string_view::npos) break;
    bold = !bold;
    position = marker + 2;
  }
}

std::vector<std::string_view> boundedLines(std::string_view text, size_t maximum, bool tail) {
  std::vector<std::string_view> lines;
  size_t position = 0;
  while (position <= text.size() && maximum > 0) {
    const size_t newline = text.find('\n', position);
    const size_t end = newline == std::string_view::npos ? text.size() : newline;
    if (lines.size() == maximum) {
      if (!tail) break;
      lines.erase(lines.begin());
    }
    lines.push_back(text.substr(position, end - position));
    if (newline == std::string_view::npos) break;
    position = newline + 1;
  }
  return lines;
}

}  // namespace

std::string sanitizeText(std::string_view input, bool multiline, size_t max_codepoints) {
  std::string out;
  out.reserve(std::min(input.size(), max_codepoints * 4));
  size_t position = 0;
  size_t count = 0;
  while (position < input.size() && count < max_codepoints) {
    const unsigned char byte = static_cast<unsigned char>(input[position]);
    if (byte == '\r' || byte == '\n') {
      if (byte == '\r' && position + 1 < input.size() && input[position + 1] == '\n') ++position;
      out.push_back(multiline ? '\n' : ' ');
      ++position;
      ++count;
      continue;
    }
    if (byte == '\t') {
      out.push_back(' ');
      ++position;
      ++count;
      continue;
    }
    if (byte < 0x20 || byte == 0x7f) {
      ++position;
      continue;
    }
    const size_t length = validCharLength(input, position);
    if (length == 0) {
      ++position;
      continue;
    }
    out.append(input.substr(position, length));
    position += length;
    ++count;
  }
  return out;
}

int32_t measureText(FontManager& font, std::string_view text, int16_t scale) {
  if (scale < 1 || text.empty()) return 0;
  int64_t width = 0;
  size_t position = 0;
  while (position < text.size()) {
    size_t end = position;
    size_t codepoints = 0;
    while (end < text.size() && codepoints < 64) {
      const size_t length = validCharLength(text, end);
      end += length == 0 ? 1 : length;
      ++codepoints;
    }
    const std::string chunk(text.substr(position, end - position));
    const int16_t chunk_width = font.uiTextWidth(chunk.c_str());
    if (chunk_width > 0) width += static_cast<int64_t>(chunk_width) * scale;
    position = end;
  }
  return static_cast<int32_t>(std::min<int64_t>(width, INT32_MAX));
}

std::string ellipsize(FontManager& font, std::string_view text, int16_t max_width,
                      int16_t scale) {
  return ellipsizeValid(font, sanitizeText(text, false), max_width, scale);
}

void drawTextFit(PortraitCanvas& canvas, FontManager& font, int16_t x, int16_t y,
                 int16_t max_width, std::string_view text, int16_t scale, bool bold,
                 uint8_t gray) {
  const std::string fitted = ellipsizeValid(font, sanitizeText(text, false), max_width, scale);
  drawRaw(canvas, font, x, y, fitted, scale, bold, gray);
}

size_t drawTextWrapped(PortraitCanvas& canvas, FontManager& font, const Rect& bounds,
                       std::string_view text, int16_t scale, int16_t line_height,
                       size_t max_lines, uint8_t gray) {
  if (bounds.empty() || line_height <= 0) return 0;
  const std::string clean = sanitizeText(text, true);
  bool truncated = false;
  const std::vector<std::string> lines =
      wrapValid(font, clean, bounds.w, scale, max_lines, truncated);
  size_t drawn = 0;
  for (const std::string& line : lines) {
    const int32_t y = static_cast<int32_t>(bounds.y) + drawn * line_height;
    if (y >= bounds.bottom()) break;
    drawRaw(canvas, font, bounds.x, y, line, scale, false, gray);
    ++drawn;
  }
  return drawn;
}

namespace {

bool isAlertLine(std::string_view line) {
  for (const char* tag : {"WARN", "ERROR", "ERR ", "CRIT", "FATAL", "FAIL"}) {
    if (line.find(tag) != std::string_view::npos) return true;
  }
  return false;
}

}  // namespace

void drawTextPanel(PortraitCanvas& canvas, FontManager& font, const PanelSpec& panel,
                   const Rect& bounds, const VariableValues& variables) {
  if (bounds.empty()) return;
  std::string interpolated;
  std::string error;
  if (!interpolateVariables(panel.text.content, variables, interpolated, error)) {
    interpolated = "Invalid text";
  }
  const std::string content = sanitizeText(interpolated, true);
  if (content.find_first_not_of(" \n") == std::string::npos) {
    style::emptyState(canvas, font, bounds, "No text");
    return;
  }
  const std::string& variant = panel.style.variant;
  const bool multiline = variant == "multiline" || variant == "description" ||
                         variant == "log_tail" || variant == "markdown" ||
                         (variant == "default" && panel.text.multiline);
  const bool multi_rows = multiline || variant == "key_value";
  const int16_t caption = font.uiFontHeight();
  if (caption <= 0) return;
  size_t max_lines = panel.text.max_lines;
  if (multi_rows && !panel.text.multiline && max_lines == 1) max_lines = 8;
  max_lines = std::max<size_t>(max_lines, 1);

  if (variant == "key_value") {
    std::vector<std::string_view> rows;
    for (std::string_view row : boundedLines(content, 64, false)) {
      if (row.find_first_not_of(' ') != std::string_view::npos) rows.push_back(row);
    }
    if (rows.size() > max_lines) rows.resize(max_lines);
    // Body size when every row fits comfortably, else caption.
    const int16_t body_row = static_cast<int16_t>(caption * kScaleBody + 12);
    const int16_t scale = bounds.w >= 280 && rows.size() * body_row <= static_cast<size_t>(bounds.h)
                              ? kScaleBody : kScaleCaption;
    const int16_t row_h = scale == kScaleBody ? body_row : static_cast<int16_t>(caption + 10);
    const size_t count = std::min<size_t>(rows.size(), bounds.h / row_h);
    int32_t key_w = 0;
    std::vector<std::pair<std::string_view, std::string_view>> pairs;
    for (size_t i = 0; i < count; ++i) {
      const size_t separator = rows[i].find_first_of(":=");
      std::string_view key = separator == std::string_view::npos ? std::string_view{} : rows[i].substr(0, separator);
      std::string_view value = separator == std::string_view::npos ? rows[i] : rows[i].substr(separator + 1);
      while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
      while (!key.empty() && key.back() == ' ') key.remove_suffix(1);
      key_w = std::max(key_w, measureText(font, key, scale));
      pairs.emplace_back(key, value);
    }
    const int16_t label_w = static_cast<int16_t>(std::min<int32_t>(key_w + 16, bounds.w * 2 / 5));
    for (size_t i = 0; i < pairs.size(); ++i) {
      const int16_t y = static_cast<int16_t>(bounds.y + i * row_h);
      const int16_t ty = static_cast<int16_t>(y + (row_h - caption * scale) / 2 - 1);
      style::text(canvas, font, bounds.x, ty, static_cast<int16_t>(label_w - 8), pairs[i].first, scale);
      style::text(canvas, font, static_cast<int16_t>(bounds.x + label_w), ty,
                  static_cast<int16_t>(bounds.w - label_w), pairs[i].second, scale, true);
      if (i + 1 < pairs.size())
        style::dottedHLine(canvas, bounds.x, static_cast<int16_t>(bounds.right() - 1),
                           static_cast<int16_t>(y + row_h - 1), 2);
    }
    return;
  }

  if (variant == "log_tail") {
    const int16_t row_h = static_cast<int16_t>(caption + 5);
    const size_t lines = std::min<size_t>(max_lines, bounds.h / row_h);
    const std::vector<std::string_view> rows = boundedLines(content, lines, true);
    for (size_t i = 0; i < rows.size(); ++i) {
      const int16_t y = static_cast<int16_t>(bounds.y + i * row_h);
      const bool alert = isAlertLine(rows[i]);
      if (alert) canvas.fillRect(bounds.x, static_cast<int16_t>(y + 1), 3, caption, 0);
      style::text(canvas, font, static_cast<int16_t>(bounds.x + 8), y,
                  static_cast<int16_t>(bounds.w - 8), rows[i], kScaleCaption, alert);
    }
    return;
  }

  if (variant == "markdown") {
    const int16_t line_height = static_cast<int16_t>(caption + 5);
    const size_t lines = std::min<size_t>(max_lines, bounds.h / line_height);
    const std::vector<std::string_view> rows = boundedLines(content, 64, false);
    size_t drawn = 0;
    for (const std::string_view source : rows) {
      if (drawn >= lines) break;
      std::string_view row = source;
      const bool rule = row.size() >= 3 && row.find_first_not_of("- ") == std::string_view::npos &&
                        row.find('-') != std::string_view::npos;
      const int16_t y = static_cast<int16_t>(bounds.y + drawn * line_height);
      if (rule) {
        style::dottedHLine(canvas, bounds.x, static_cast<int16_t>(bounds.right() - 1),
                           static_cast<int16_t>(y + line_height / 2), 2);
      } else if (row.substr(0, 2) == "- " || row.substr(0, 2) == "* ") {
        canvas.fillRect(static_cast<int16_t>(bounds.x + 2), static_cast<int16_t>(y + caption / 2 - 2), 4, 4, 0);
        drawMarkdownInline(canvas, font, static_cast<int16_t>(bounds.x + 12), y,
                           static_cast<int16_t>(bounds.w - 12), row.substr(2), kScaleCaption);
      } else {
        drawMarkdownInline(canvas, font, bounds.x, y, bounds.w, row, kScaleCaption);
      }
      ++drawn;
    }
    return;
  }

  if (multiline) {
    // Body size when the whole text fits at it, else caption.
    const int16_t body_line = static_cast<int16_t>(caption * kScaleBody + 6);
    if (bounds.w >= 240) {
      bool truncated = false;
      const size_t room = std::min<size_t>(max_lines, bounds.h / body_line);
      wrapValid(font, content, bounds.w, kScaleBody, room, truncated);
      if (room > 0 && !truncated) {
        drawTextWrapped(canvas, font, bounds, content, kScaleBody, body_line, room, 0);
        return;
      }
    }
    const int16_t line_height = static_cast<int16_t>(caption + 5);
    drawTextWrapped(canvas, font, bounds, content, kScaleCaption, line_height,
                    std::min<size_t>(max_lines, bounds.h / line_height), 0);
    return;
  }

  // Single line: largest of headline/body/caption that fits.
  for (const int16_t scale : {static_cast<int16_t>(3), kScaleBody, kScaleCaption}) {
    if (caption * scale > bounds.h) continue;
    if (scale > kScaleCaption && measureText(font, content, scale) > bounds.w) continue;
    style::text(canvas, font, bounds.x, bounds.y, bounds.w, content, scale, scale > kScaleCaption);
    return;
  }
}

}  // namespace eego::dashboard::ui
