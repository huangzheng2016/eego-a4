// SPDX-License-Identifier: GPL-3.0-or-later
#include "dashboard_ui/text.h"

#include <algorithm>
#include <vector>

namespace eego::dashboard::ui {
namespace {

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

size_t boundaryBefore(std::string_view text, size_t offset) {
  offset = std::min(offset, text.size());
  while (offset > 0 && offset < text.size() &&
         (static_cast<unsigned char>(text[offset]) & 0xc0) == 0x80) {
    --offset;
  }
  return offset;
}

std::string ellipsizeValid(FontManager& font, std::string_view text, int16_t max_width,
                           int16_t scale) {
  if (max_width <= 0) return {};
  if (measureText(font, text, scale) <= max_width) return std::string(text);
  if (measureText(font, "...", scale) > max_width) return {};
  size_t low = 0;
  size_t high = text.size();
  while (low < high) {
    size_t middle = boundaryBefore(text, low + (high - low + 1) / 2);
    const std::string candidate = std::string(text.substr(0, middle)) + "...";
    if (measureText(font, candidate, scale) <= max_width) {
      low = middle;
    } else {
      high = middle == 0 ? 0 : middle - 1;
    }
  }
  return std::string(text.substr(0, boundaryBefore(text, low))) + "...";
}

size_t longestFit(FontManager& font, std::string_view text, size_t begin, size_t end,
                  int16_t width, int16_t scale) {
  size_t low = begin;
  size_t high = end;
  while (low < high) {
    const size_t middle = boundaryBefore(text, low + (high - low + 1) / 2);
    if (middle <= begin) return begin;
    if (measureText(font, text.substr(begin, middle - begin), scale) <= width) {
      low = middle;
    } else {
      high = middle - 1;
    }
  }
  return boundaryBefore(text, low);
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
      complete = last_paragraph && cursor >= end;
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

void drawTextPanel(PortraitCanvas& canvas, FontManager& font, const PanelSpec& panel,
                   const Rect& bounds, const VariableValues& variables) {
  if (bounds.empty()) return;
  std::string interpolated;
  std::string error;
  if (!interpolateVariables(panel.text.content, variables, interpolated, error)) {
    interpolated = "Invalid text";
  }
  const std::string content = sanitizeText(interpolated, true);
  const std::string& variant = panel.style.variant;
  const bool multiline = variant == "multiline" || variant == "description" ||
                         variant == "log_tail" || variant == "markdown" ||
                         (variant == "default" && panel.text.multiline);
  const bool multi_rows = multiline || variant == "key_value";
  const int16_t scale = !multiline && variant != "key_value" && bounds.h >= 32 ? 2 : 1;
  const int16_t line_height = font.uiFontHeight() * scale + 4;
  if (line_height <= 0) return;
  size_t max_lines = panel.text.max_lines;
  if (multi_rows && !panel.text.multiline && max_lines == 1) max_lines = 8;
  size_t lines = std::min<size_t>(max_lines, bounds.h / line_height);
  if (!multi_rows) lines = std::min<size_t>(lines, 1);
  if (lines == 0) return;

  if (variant == "key_value") {
    const std::vector<std::string_view> rows = boundedLines(content, lines, false);
    const int16_t label_width = bounds.w * 2 / 5;
    for (size_t i = 0; i < rows.size(); ++i) {
      const size_t separator = rows[i].find_first_of(":=");
      const std::string_view key = separator == std::string_view::npos ? std::string_view{} : rows[i].substr(0, separator);
      const std::string_view value = separator == std::string_view::npos ? rows[i] : rows[i].substr(separator + 1);
      const int16_t y = bounds.y + i * line_height;
      drawTextFit(canvas, font, bounds.x, y, label_width - 4, key, scale, false, 6);
      drawTextFit(canvas, font, bounds.x + label_width, y, bounds.w - label_width, value,
                  scale, false, 0);
    }
    return;
  }

  if (variant == "log_tail") {
    const std::vector<std::string_view> rows = boundedLines(content, lines, true);
    for (size_t i = 0; i < rows.size(); ++i) {
      drawTextFit(canvas, font, bounds.x, bounds.y + i * line_height, bounds.w, rows[i],
                  scale, false, 0);
    }
    return;
  }

  if (variant == "markdown") {
    const std::vector<std::string_view> rows = boundedLines(content, 64, false);
    size_t drawn = 0;
    for (const std::string_view source : rows) {
      if (drawn >= lines) break;
      std::string_view row = source;
      const bool rule = row.size() >= 3 && row.find_first_not_of("- ") == std::string_view::npos &&
                        row.find('-') != std::string_view::npos;
      const int16_t y = bounds.y + drawn * line_height;
      if (rule) {
        canvas.drawLine(bounds.x, y + line_height / 2, bounds.x + bounds.w - 1,
                        y + line_height / 2, 0);
      } else {
        if (row.substr(0, 2) == "- " || row.substr(0, 2) == "* ") {
          drawRaw(canvas, font, bounds.x, y, "-", scale, false, 0);
          drawMarkdownInline(canvas, font, bounds.x + 12, y, bounds.w - 12, row.substr(2), scale);
        } else {
          drawMarkdownInline(canvas, font, bounds.x, y, bounds.w, row, scale);
        }
      }
      ++drawn;
    }
    return;
  }

  if (multiline) {
    drawTextWrapped(canvas, font, bounds, content, scale, line_height, lines, 0);
  } else {
    drawTextFit(canvas, font, bounds.x, bounds.y, bounds.w, content, scale, false, 0);
  }
}

}  // namespace eego::dashboard::ui
