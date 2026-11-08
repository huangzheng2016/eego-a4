// SPDX-License-Identifier: GPL-3.0-or-later
//
//                                  dataLen(u16),dataOff(u32)
//   [kern / ligature ...]
//
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include "portrait_canvas.h"

namespace eego {
class CpFontRenderer {
public:
    struct FontInfo {
        uint32_t fileBytes = 0;
        uint32_t intervalCount = 0;
        uint32_t glyphCount = 0;
        uint8_t advanceY = 0;
        int16_t ascender = 0;
        int16_t descender = 0;
        bool is2Bit = false;
        uint8_t styleId = 0;
    };

    CpFontRenderer() = default;
    ~CpFontRenderer();

    CpFontRenderer(const CpFontRenderer&) = delete;
    CpFontRenderer& operator=(const CpFontRenderer&) = delete;

    bool load(const char* path);
    bool loadMemory(const uint8_t* data, size_t bytes);
    void unload();
    bool loaded() const { return loaded_; }
    const FontInfo& info() const { return info_; }

    bool hasCodepoint(uint32_t codepoint) const;
    uint16_t countMissing(const char* utf8Text) const;

    int16_t drawText(PortraitCanvas& canvas, int16_t x, int16_t top,
                     const char* utf8Text, int16_t scale = 1, bool bold = false);

    int16_t drawTextGray(PortraitCanvas& canvas, int16_t x, int16_t top,
                         const char* utf8Text, int16_t scale, uint8_t gray);

    int16_t textWidth(const char* utf8Text);

    int16_t fontHeight() const;

private:
    struct Interval {
        uint32_t first;
        uint32_t last;
        uint32_t offset;
    };
    struct Glyph {
        uint8_t width = 0;
        uint8_t height = 0;
        uint16_t advanceX = 0;
        int16_t left = 0;
        int16_t top = 0;
        uint16_t dataLength = 0;
        uint32_t dataOffset = 0;
    };

    FontInfo info_{};
    Interval* intervals_ = nullptr;
    uint8_t* bitmap_ = nullptr;
    size_t bitmapCapacity_ = 0;
    FILE* file_ = nullptr;
    const uint8_t* memory_ = nullptr;
    size_t memorySize_ = 0;
    uint32_t glyphsFileOffset_ = 0;
    uint32_t bitmapFileOffset_ = 0;
    bool loaded_ = false;

    static uint16_t readU16(const uint8_t* data);
    static int16_t readI16(const uint8_t* data);
    static uint32_t readU32(const uint8_t* data);
    static bool nextCodepoint(const char*& cursor, uint32_t& codepoint);

    bool loadMetadata(const char* sourceName);
    bool readAt(uint32_t offset, void* destination, size_t bytes);
    int32_t findGlyphIndex(uint32_t codepoint) const;
    bool readGlyph(uint32_t glyphIndex, Glyph& glyph);
    bool ensureBitmapCapacity(size_t bytes);
    void drawGlyph(PortraitCanvas& canvas, int16_t cursorX, int16_t baseline,
                   const Glyph& glyph, int16_t scale, uint8_t gray);
    int16_t drawOnce(PortraitCanvas& canvas, int16_t x, int16_t top,
                     const char* utf8Text, int16_t scale, uint8_t gray);
};

}  // namespace eego
