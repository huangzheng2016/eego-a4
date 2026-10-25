// SPDX-License-Identifier: GPL-3.0-or-later
// Host replacement for eego::FontManager used by the render harness.
#pragma once

#include <string>

namespace eego::render_host {

// Fallback: the firmware's built-in 5x7 font (fallback_font.cpp, compiled
// unmodified). FreeType: a TTF/OTF/TTC rasterised to 1-bit at `pixel_size`,
// binarised the same way CpFontRenderer binarises 2-bit glyphs (any ink -> black),
// approximating the embedded Noto Sans CJK CPFONT.
bool useFallbackFont();
bool useFreeTypeFont(const std::string& path, int pixel_size, std::string& error);
int fontLineHeight();

}  // namespace eego::render_host
