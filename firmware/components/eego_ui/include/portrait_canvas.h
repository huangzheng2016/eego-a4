// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "uc8279c_display.h"
#include "eego_pins.h"

#include <stdint.h>

namespace eego {
class PortraitCanvas {
public:
    explicit PortraitCanvas(Uc8279cDisplay& display);

    void clear();

    void drawPixel(int16_t x, int16_t y, uint8_t color);
    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color);
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color);
    void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t color);
    void drawCircle(int16_t cx, int16_t cy, int16_t r, uint8_t color);
    void fillCircle(int16_t cx, int16_t cy, int16_t r, uint8_t color);

    void drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint8_t color);
    void fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint8_t color);
    void fillRoundRectGray(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint8_t gray);

    void drawIcon(int16_t x, int16_t y, const uint8_t* icon, int16_t w, int16_t h, uint8_t color);

    void drawTextCentered(int16_t x, int16_t y, int16_t w, const char* text, int16_t scale, bool bold);
    void drawTextRight(int16_t x, int16_t y, int16_t w, const char* text, int16_t scale, bool bold);

    void drawPixelGray(int16_t x, int16_t y, uint8_t gray);

    void setClip(int16_t x, int16_t y, int16_t w, int16_t h);
    void resetClip();
    void useUiContentRect();

    void display(RefreshMode mode = RefreshMode::Fast);
    void requestFullRefresh();
    void waitRefreshComplete();

    static constexpr uint16_t WIDTH = 552;
    static constexpr uint16_t HEIGHT = 768;

private:
    Uc8279cDisplay& display_;
    int16_t clipX_, clipY_, clipW_, clipH_;
    bool clipEnabled_;

    bool inClip(int16_t x, int16_t y) const;

    void drawCornerArc(int16_t cx, int16_t cy, int16_t r, int quadrant, uint8_t color);
};

}  // namespace eego
