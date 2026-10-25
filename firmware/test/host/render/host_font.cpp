// SPDX-License-Identifier: GPL-3.0-or-later
// Host implementation of eego::FontManager for the render harness. Mirrors the
// device contract: y is the text top, glyph pixels are magnified by an integer
// scale into scale x scale blocks, bold overdraws at x+1, widths are unscaled.
#include "host_font.h"

#include <cstring>
#include <unordered_map>
#include <vector>

#include "fallback_font.h"
#include "font_manager.h"

#ifdef EEGO_RENDER_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

namespace eego {
namespace render_host {
namespace {

struct Glyph {
  int width = 0;
  int height = 0;
  int left = 0;
  int top = 0;
  int advance = 0;
  std::vector<uint8_t> ink;  // width*height, 1 = ink
};

struct State {
  bool freetype = false;
  int ascender = 0;
  int line_height = FALLBACK_GLYPH_H;
#ifdef EEGO_RENDER_FREETYPE
  FT_Library library = nullptr;
  FT_Face face = nullptr;
#endif
  std::unordered_map<uint32_t, Glyph> cache;
};

State& state() {
  static State s;
  return s;
}

uint32_t nextCodepoint(const uint8_t*& p) {
  const uint8_t c = *p++;
  if (c < 0x80) return c;
  int extra = 0;
  uint32_t cp = 0;
  if ((c & 0xe0) == 0xc0) { extra = 1; cp = c & 0x1f; }
  else if ((c & 0xf0) == 0xe0) { extra = 2; cp = c & 0x0f; }
  else if ((c & 0xf8) == 0xf0) { extra = 3; cp = c & 0x07; }
  else return 0xfffd;
  for (int i = 0; i < extra; ++i) {
    if ((*p & 0xc0) != 0x80) return 0xfffd;
    cp = (cp << 6) | (*p++ & 0x3f);
  }
  return cp;
}

const Glyph& glyphFor(uint32_t cp) {
  State& s = state();
  auto it = s.cache.find(cp);
  if (it != s.cache.end()) return it->second;
  Glyph g;
#ifdef EEGO_RENDER_FREETYPE
  if (FT_Load_Char(s.face, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) == 0) {
    const FT_GlyphSlot slot = s.face->glyph;
    g.width = static_cast<int>(slot->bitmap.width);
    g.height = static_cast<int>(slot->bitmap.rows);
    g.left = slot->bitmap_left;
    g.top = slot->bitmap_top;
    g.advance = static_cast<int>((slot->advance.x + 32) >> 6);
    g.ink.resize(static_cast<size_t>(g.width) * g.height);
    for (int y = 0; y < g.height; ++y) {
      for (int x = 0; x < g.width; ++x) {
        // CPFONT 2-bit levels: level >= 1 is drawn black.
        g.ink[y * g.width + x] = slot->bitmap.buffer[y * slot->bitmap.pitch + x] >= 48 ? 1 : 0;
      }
    }
  }
#endif
  return s.cache.emplace(cp, std::move(g)).first->second;
}

int16_t drawFreeType(PortraitCanvas& canvas, int16_t x, int16_t top, const char* utf8,
                     int16_t scale, uint8_t gray) {
  State& s = state();
  if (scale < 1) scale = 1;
  const int baseline = top + s.ascender * scale;
  int cursor = x;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(utf8);
  while (*p) {
    const Glyph& g = glyphFor(nextCodepoint(p));
    const int ox = cursor + g.left * scale;
    const int oy = baseline - g.top * scale;
    for (int gy = 0; gy < g.height; ++gy) {
      for (int gx = 0; gx < g.width; ++gx) {
        if (!g.ink[gy * g.width + gx]) continue;
        for (int sy = 0; sy < scale; ++sy)
          for (int sx = 0; sx < scale; ++sx) {
            const int16_t px = static_cast<int16_t>(ox + gx * scale + sx);
            const int16_t py = static_cast<int16_t>(oy + gy * scale + sy);
            if (gray == 0) canvas.drawPixel(px, py, 0);
            else canvas.drawPixelGray(px, py, gray);
          }
      }
    }
    cursor += g.advance * scale;
  }
  return static_cast<int16_t>(cursor);
}

int16_t widthFreeType(const char* utf8) {
  int width = 0;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(utf8);
  while (*p) width += glyphFor(nextCodepoint(p)).advance;
  return static_cast<int16_t>(width);
}

}  // namespace

bool useFallbackFont() {
  State& s = state();
  s.freetype = false;
  s.line_height = FALLBACK_GLYPH_H;
  s.cache.clear();
  return true;
}

bool useFreeTypeFont(const std::string& path, int pixel_size, std::string& error) {
#ifdef EEGO_RENDER_FREETYPE
  State& s = state();
  s.cache.clear();
  if (s.library == nullptr && FT_Init_FreeType(&s.library) != 0) {
    error = "FT_Init_FreeType failed";
    return false;
  }
  if (s.face != nullptr) {
    FT_Done_Face(s.face);
    s.face = nullptr;
  }
  // Prefer the Simplified Chinese face of a CJK collection.
  FT_Face probe = nullptr;
  long chosen = 0;
  if (FT_New_Face(s.library, path.c_str(), -1, &probe) == 0) {
    const long faces = probe->num_faces;
    FT_Done_Face(probe);
    for (long i = 0; i < faces; ++i) {
      FT_Face face = nullptr;
      if (FT_New_Face(s.library, path.c_str(), i, &face) != 0) continue;
      const bool sc = face->family_name != nullptr && std::strstr(face->family_name, " SC") != nullptr;
      FT_Done_Face(face);
      if (sc) {
        chosen = i;
        break;
      }
    }
  }
  if (FT_New_Face(s.library, path.c_str(), chosen, &s.face) != 0) {
    error = "cannot open font " + path;
    return false;
  }
  FT_Set_Pixel_Sizes(s.face, 0, static_cast<FT_UInt>(pixel_size));
  s.ascender = static_cast<int>((s.face->size->metrics.ascender + 32) >> 6);
  const int descender = static_cast<int>((-s.face->size->metrics.descender + 32) >> 6);
  s.line_height = s.ascender + descender;
  s.freetype = true;
  return true;
#else
  (void)path;
  (void)pixel_size;
  error = "built without FreeType";
  return false;
#endif
}

int fontLineHeight() { return state().line_height; }

}  // namespace render_host

// ── eego::FontManager (host) ─────────────────────────────────────────────────

FontManager& FontManager::instance() {
  static FontManager inst;
  return inst;
}
FontManager::~FontManager() = default;

bool FontManager::loadUiFont(const char*) { return false; }
bool FontManager::loadUiFontEmbedded() { return false; }
bool FontManager::loadBodyFont(const char*) { return false; }
FontFormat FontManager::detectFormat(const char*) { return FontFormat::Unknown; }

void FontManager::drawText(PortraitCanvas& canvas, int16_t x, int16_t y, const char* utf8,
                           int16_t scale, bool bold) {
  if (!utf8) return;
  if (render_host::state().freetype) {
    render_host::drawFreeType(canvas, x, y, utf8, scale, 0);
    if (bold) render_host::drawFreeType(canvas, static_cast<int16_t>(x + 1), y, utf8, scale, 0);
  } else {
    fallbackDrawText(canvas, x, y, utf8, scale, 0);
    if (bold) fallbackDrawText(canvas, static_cast<int16_t>(x + 1), y, utf8, scale, 0);
  }
}

void FontManager::drawTextBody(PortraitCanvas& canvas, int16_t x, int16_t y, const char* utf8,
                               int16_t scale, bool bold) {
  drawText(canvas, x, y, utf8, scale, bold);
}

void FontManager::drawTextGray(PortraitCanvas& canvas, int16_t x, int16_t y, const char* utf8,
                               int16_t scale, uint8_t gray) {
  if (!utf8) return;
  if (render_host::state().freetype) render_host::drawFreeType(canvas, x, y, utf8, scale, gray);
  else fallbackDrawText(canvas, x, y, utf8, scale, gray);
}

int16_t FontManager::uiFontHeight() const {
  return static_cast<int16_t>(render_host::state().line_height);
}
int16_t FontManager::uiFontAscender() const {
  const render_host::State& s = render_host::state();
  return static_cast<int16_t>(s.freetype ? s.ascender : FALLBACK_GLYPH_H);
}
int16_t FontManager::bodyFontHeight() const { return uiFontHeight(); }
int16_t FontManager::uiTextWidth(const char* utf8) const {
  if (!utf8) return 0;
  return render_host::state().freetype ? render_host::widthFreeType(utf8)
                                       : fallbackTextWidth(utf8, 1);
}
int16_t FontManager::bodyTextWidth(const char* utf8) const { return uiTextWidth(utf8); }
bool FontManager::uiLoaded() const { return render_host::state().freetype; }
bool FontManager::bodyLoaded() const { return false; }

}  // namespace eego
