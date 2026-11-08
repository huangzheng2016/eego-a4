// SPDX-License-Identifier: GPL-3.0-or-later
//
#pragma once

#include <stdint.h>

#include "portrait_canvas.h"

namespace eego {
class EdcFont;
class CpFontRenderer;

enum class FontFormat { Unknown, EdcbookBin, CpFontV4 };

class FontManager {
public:
    static FontManager& instance();

    bool loadUiFont(const char* path);
    bool loadUiFontEmbedded();
    bool loadBodyFont(const char* path);

    static FontFormat detectFormat(const char* path);

    void drawText(PortraitCanvas& canvas, int16_t x, int16_t y,
                  const char* utf8, int16_t scale = 1, bool bold = false);
    void drawTextBody(PortraitCanvas& canvas, int16_t x, int16_t y,
                      const char* utf8, int16_t scale = 1, bool bold = false);
    void drawTextGray(PortraitCanvas& canvas, int16_t x, int16_t y,
                      const char* utf8, int16_t scale, uint8_t gray);

    int16_t uiFontHeight() const;
    int16_t uiFontAscender() const;
    int16_t bodyFontHeight() const;
    int16_t uiTextWidth(const char* utf8) const;
    int16_t bodyTextWidth(const char* utf8) const;

    bool uiLoaded() const;
    bool bodyLoaded() const;
    FontFormat uiFormat() const { return uiFmt_; }
    FontFormat bodyFormat() const { return bodyFmt_; }

private:
    FontManager() = default;
    ~FontManager();
    FontManager(const FontManager&) = delete;
    FontManager& operator=(const FontManager&) = delete;

    void unloadUi();
    void unloadBody();
    bool loadInto(const char* path, FontFormat& fmtOut,
                  EdcFont*& edcOut, CpFontRenderer*& cpOut);

    EdcFont*        uiEdc_ = nullptr;
    CpFontRenderer* uiCp_  = nullptr;
    FontFormat      uiFmt_ = FontFormat::Unknown;

    EdcFont*        bodyEdc_ = nullptr;
    CpFontRenderer* bodyCp_  = nullptr;
    FontFormat      bodyFmt_ = FontFormat::Unknown;
};

}  // namespace eego
