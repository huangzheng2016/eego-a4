// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "dashboard_core/model.h"
#include "dashboard_ui/geometry.h"
#include "font_manager.h"
#include "portrait_canvas.h"

namespace eego::dashboard::ui {

std::string sanitizeText(std::string_view input, bool multiline = true,
                         size_t max_codepoints = kMaxTextCodePoints);
int32_t measureText(FontManager& font, std::string_view text, int16_t scale = 1);
std::string ellipsize(FontManager& font, std::string_view text, int16_t max_width,
                      int16_t scale = 1);
void drawTextFit(PortraitCanvas& canvas, FontManager& font, int16_t x, int16_t y,
                 int16_t max_width, std::string_view text, int16_t scale = 1,
                 bool bold = false, uint8_t gray = 0);
size_t drawTextWrapped(PortraitCanvas& canvas, FontManager& font, const Rect& bounds,
                       std::string_view text, int16_t scale, int16_t line_height,
                       size_t max_lines, uint8_t gray = 0);
void drawTextPanel(PortraitCanvas& canvas, FontManager& font, const PanelSpec& panel,
                   const Rect& bounds, const VariableValues& variables);

}  // namespace eego::dashboard::ui
