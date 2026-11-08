// SPDX-License-Identifier: GPL-3.0-or-later
//
//
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include <map>
#include <vector>

#include "portrait_canvas.h"

namespace eego {
class EdcFont {
public:
    EdcFont() = default;
    ~EdcFont();

    EdcFont(const EdcFont&) = delete;
    EdcFont& operator=(const EdcFont&) = delete;

    bool load(const char* path);
    void unload();
    bool loaded() const { return loaded_; }
    int  fontHeight() const { return font_height_; }

    int findGlyph(uint16_t cp) const;

    bool decodeGlyph(int idx, std::vector<uint8_t>& out,
                     int& w, int& h, int& xo, int& yo, int& adv);

    int textWidth(const char* utf8);

    int drawText(PortraitCanvas& canvas, const char* utf8,
                 int x, int y, int scale = 1, bool bold = false);

    int drawTextGray(PortraitCanvas& canvas, const char* utf8,
                     int x, int y, int scale, uint8_t gray);

    static uint32_t nextCodepoint(const uint8_t*& p);

private:
    struct Entry {
        uint16_t cp;
        uint16_t adv;
        uint8_t  w, h;
        int8_t   xo, yo;
        uint32_t off, size;
    };
    struct GCEnt {
        int w, h, xo, yo, adv;
        uint8_t* pix;
    };

    std::map<uint16_t, GCEnt> cache_;
    size_t cache_bytes_ = 0;
    static const size_t CACHE_CAP = 1536 * 1024;

    bool      loaded_ = false;
    int       font_height_ = 0;
    uint32_t  char_count_ = 0;
    Entry*    entries_ = nullptr;
    FILE*     file_ = nullptr;
    char      path_[160] = {};

    bool readAt(uint32_t off, uint8_t* buf, size_t n);
    int  drawOnce(PortraitCanvas& canvas, const char* utf8,
                  int x, int y, int scale, uint8_t gray);
};

}  // namespace eego
