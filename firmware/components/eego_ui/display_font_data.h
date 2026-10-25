// SPDX-License-Identifier: GPL-3.0-or-later
// Data layout of the generated display fonts (display_font_data.cpp).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace eego::display_font_data {

struct DisplayGlyph {
    uint32_t codepoint;
    uint32_t offset;   // into the font's RLE data
    uint8_t width;
    uint8_t height;
    int8_t left;       // bearing from the pen position
    int8_t top;        // baseline -> glyph top
    uint8_t advance;
};

struct DisplayFontData {
    int16_t cap;       // nominal cap/digit height (px)
    int16_t ascent;    // number line: max digit/sign top above baseline
    int16_t descent;   // number line: max digit/sign extent below baseline
    uint16_t glyph_count;
    const DisplayGlyph* glyphs;  // sorted by codepoint
    const uint8_t* data;
};

extern const DisplayFontData kFonts[];
extern const size_t kFontCount;

}  // namespace eego::display_font_data
