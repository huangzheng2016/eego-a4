// SPDX-License-Identifier: GPL-3.0-or-later
#include "display_font.h"

#include "display_font_data.h"
#include "font_manager.h"

namespace eego::display_font {
namespace {

using display_font_data::DisplayFontData;
using display_font_data::DisplayGlyph;

constexpr uint8_t kGrayForLevel[4] = {15, 10, 5, 0};

const DisplayFontData* font(size_t size) {
    if (display_font_data::kFontCount == 0) return nullptr;
    if (size >= display_font_data::kFontCount) size = display_font_data::kFontCount - 1;
    return &display_font_data::kFonts[size];
}

uint32_t nextCodepoint(const uint8_t*& p) {
    const uint8_t c = *p++;
    if (c < 0x80) return c;
    int extra = 0;
    uint32_t cp = 0;
    if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
    else return 0xFFFD;
    for (int i = 0; i < extra; ++i) {
        if ((*p & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (*p++ & 0x3F);
    }
    return cp;
}

const DisplayGlyph* findGlyph(const DisplayFontData& f, uint32_t cp) {
    size_t low = 0;
    size_t high = f.glyph_count;
    while (low < high) {
        const size_t mid = (low + high) / 2;
        if (f.glyphs[mid].codepoint < cp) low = mid + 1;
        else high = mid;
    }
    return low < f.glyph_count && f.glyphs[low].codepoint == cp ? &f.glyphs[low] : nullptr;
}

// UI 字体回退字符的放大倍数：行高接近显示字号。
int16_t fallbackScale(const DisplayFontData& f) {
    const int16_t ui = FontManager::instance().uiFontHeight();
    if (ui <= 0) return 1;
    const int16_t scale = static_cast<int16_t>((f.ascent + f.descent + ui / 2) / ui);
    return scale < 1 ? 1 : scale;
}

// 将码点编码为 UTF-8（回退绘制用）。
void encodeUtf8(uint32_t cp, char out[5]) {
    if (cp < 0x80) { out[0] = static_cast<char>(cp); out[1] = 0; return; }
    if (cp < 0x800) {
        out[0] = static_cast<char>(0xC0 | (cp >> 6));
        out[1] = static_cast<char>(0x80 | (cp & 0x3F));
        out[2] = 0;
        return;
    }
    if (cp < 0x10000) {
        out[0] = static_cast<char>(0xE0 | (cp >> 12));
        out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (cp & 0x3F));
        out[3] = 0;
        return;
    }
    out[0] = static_cast<char>(0xF0 | (cp >> 18));
    out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (cp & 0x3F));
    out[4] = 0;
}

void drawGlyph(PortraitCanvas& canvas, const DisplayFontData& f, const DisplayGlyph& g,
               int16_t penX, int16_t baseline, uint8_t ink) {
    const int16_t ox = static_cast<int16_t>(penX + g.left);
    const int16_t oy = static_cast<int16_t>(baseline - g.top);
    const uint32_t total = static_cast<uint32_t>(g.width) * g.height;
    const uint8_t* p = f.data + g.offset;
    uint32_t pos = 0;
    while (pos < total) {
        const uint8_t level = *p >> 6;
        uint32_t run = (*p & 0x3F) + 1u;
        ++p;
        if (level == 0 || (ink != 0 && level < 2)) {
            pos += run;
            continue;
        }
        // 按行切分 run：整段实心用 fillRect，灰阶逐像素。
        while (run > 0 && pos < total) {
            const uint16_t x = static_cast<uint16_t>(pos % g.width);
            const uint16_t y = static_cast<uint16_t>(pos / g.width);
            uint32_t span = g.width - x;
            if (span > run) span = run;
            const int16_t px = static_cast<int16_t>(ox + x);
            const int16_t py = static_cast<int16_t>(oy + y);
            if (ink != 0) {
                canvas.fillRect(px, py, static_cast<int16_t>(span), 1, 1);
            } else if (level == 3) {
                canvas.fillRect(px, py, static_cast<int16_t>(span), 1, 0);
            } else {
                for (uint32_t i = 0; i < span; ++i)
                    canvas.drawPixelGray(static_cast<int16_t>(px + i), py, kGrayForLevel[level]);
            }
            pos += span;
            run -= span;
        }
    }
}

}  // namespace

size_t count() { return display_font_data::kFontCount; }

int16_t capHeight(size_t size) {
    const DisplayFontData* f = font(size);
    return f ? f->cap : 0;
}

int16_t ascent(size_t size) {
    const DisplayFontData* f = font(size);
    return f ? f->ascent : 0;
}

int16_t lineHeight(size_t size) {
    const DisplayFontData* f = font(size);
    return f ? static_cast<int16_t>(f->ascent + f->descent) : 0;
}

bool covers(size_t size, const char* utf8) {
    const DisplayFontData* f = font(size);
    if (!f || !utf8) return false;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(utf8);
    while (*p) {
        if (!findGlyph(*f, nextCodepoint(p))) return false;
    }
    return true;
}

int16_t textWidth(size_t size, const char* utf8) {
    const DisplayFontData* f = font(size);
    if (!f || !utf8) return 0;
    FontManager& ui = FontManager::instance();
    const int16_t scale = fallbackScale(*f);
    int32_t width = 0;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(utf8);
    while (*p) {
        const uint32_t cp = nextCodepoint(p);
        if (const DisplayGlyph* g = findGlyph(*f, cp)) {
            width += g->advance;
        } else {
            char ch[5];
            encodeUtf8(cp, ch);
            width += ui.uiTextWidth(ch) * scale;
        }
    }
    return static_cast<int16_t>(width > INT16_MAX ? INT16_MAX : width);
}

int16_t drawText(PortraitCanvas& canvas, size_t size, int16_t x, int16_t top,
                 const char* utf8, uint8_t ink) {
    const DisplayFontData* f = font(size);
    if (!f || !utf8) return x;
    FontManager& ui = FontManager::instance();
    const int16_t baseline = static_cast<int16_t>(top + f->ascent);
    const int16_t scale = fallbackScale(*f);
    int16_t pen = x;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(utf8);
    while (*p) {
        const uint32_t cp = nextCodepoint(p);
        if (const DisplayGlyph* g = findGlyph(*f, cp)) {
            drawGlyph(canvas, *f, *g, pen, baseline, ink);
            pen = static_cast<int16_t>(pen + g->advance);
        } else {
            char ch[5];
            encodeUtf8(cp, ch);
            const int16_t y = static_cast<int16_t>(baseline - ui.uiFontAscender() * scale);
            if (ink == 0) ui.drawText(canvas, pen, y, ch, scale, true);
            else ui.drawTextGray(canvas, pen, y, ch, scale, ink);
            pen = static_cast<int16_t>(pen + ui.uiTextWidth(ch) * scale);
        }
    }
    return pen;
}

}  // namespace eego::display_font
