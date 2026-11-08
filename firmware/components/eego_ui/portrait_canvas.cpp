// SPDX-License-Identifier: GPL-3.0-or-later

#include "portrait_canvas.h"

#include "font_manager.h"
#include "icons.h"

namespace eego {
static_assert(sizeof(ICON_SETTINGS_72) == 72 * 72 / 8, "ICON_SETTINGS_72 尺寸错误");
static_assert(sizeof(ICON_CHEVRON_LEFT_32) == 32 * 32 / 8, "ICON_CHEVRON_LEFT_32 尺寸错误");
static_assert(sizeof(ICON_CHEVRON_RIGHT_16) == 16 * 16 / 8, "ICON_CHEVRON_RIGHT_16 尺寸错误");

PortraitCanvas::PortraitCanvas(Uc8279cDisplay& display)
    : display_(display),
      clipX_(0), clipY_(0), clipW_(WIDTH), clipH_(HEIGHT),
      clipEnabled_(false) {}

bool PortraitCanvas::inClip(int16_t x, int16_t y) const {
    if (!clipEnabled_) return true;
    return x >= clipX_ && x < clipX_ + clipW_ &&
           y >= clipY_ && y < clipY_ + clipH_;
}

void PortraitCanvas::setClip(int16_t x, int16_t y, int16_t w, int16_t h) {
    if (w <= 0 || h <= 0) {
        resetClip();
        return;
    }
    clipX_ = x;
    clipY_ = y;
    clipW_ = w;
    clipH_ = h;
    clipEnabled_ = true;
}

void PortraitCanvas::resetClip() {
    clipX_ = 0;
    clipY_ = 0;
    clipW_ = WIDTH;
    clipH_ = HEIGHT;
    clipEnabled_ = false;
}

void PortraitCanvas::useUiContentRect() {
    setClip(UI_CONTENT_X, UI_CONTENT_Y, UI_CONTENT_WIDTH, UI_CONTENT_HEIGHT);
}

void PortraitCanvas::clear() {
    display_.clear();
}

void PortraitCanvas::drawPixel(int16_t x, int16_t y, uint8_t color) {
    if (!inClip(x, y)) return;
    display_.drawPixel(x, y, color);
}

void PortraitCanvas::drawPixelGray(int16_t x, int16_t y, uint8_t gray) {
    if (!inClip(x, y)) return;
    display_.drawPixelGray(x, y, gray);
}

void PortraitCanvas::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color) {
    if (w <= 0 || h <= 0) return;

    int16_t x0 = x;
    int16_t y0 = y;
    int16_t x1 = x + w;  // exclusive
    int16_t y1 = y + h;

    if (clipEnabled_) {
        const int16_t cx1 = clipX_ + clipW_;
        const int16_t cy1 = clipY_ + clipH_;
        if (x0 < clipX_) x0 = clipX_;
        if (y0 < clipY_) y0 = clipY_;
        if (x1 > cx1) x1 = cx1;
        if (y1 > cy1) y1 = cy1;
        if (x0 >= x1 || y0 >= y1) return;
    }
    display_.fillRect(x0, y0, x1 - x0, y1 - y0, color);
}

void PortraitCanvas::drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color) {
    if (w <= 0 || h <= 0) return;
    fillRect(x, y, w, 1, color);                  // top
    fillRect(x, y + h - 1, w, 1, color);          // bottom
    fillRect(x, y, 1, h, color);                  // left
    fillRect(x + w - 1, y, 1, h, color);          // right
}

void PortraitCanvas::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t color) {
    const int16_t dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    const int16_t dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    const int16_t sx = (x0 < x1) ? 1 : -1;
    const int16_t sy = (y0 < y1) ? 1 : -1;
    int16_t err = dx - dy;

    while (true) {
        drawPixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        const int16_t e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

void PortraitCanvas::drawCircle(int16_t cx, int16_t cy, int16_t r, uint8_t color) {
    if (r <= 0) return;
    int16_t x = r;
    int16_t y = 0;
    int16_t err = 1 - r;

    while (x >= y) {
        drawPixel(cx + x, cy + y, color);
        drawPixel(cx - x, cy + y, color);
        drawPixel(cx + x, cy - y, color);
        drawPixel(cx - x, cy - y, color);
        drawPixel(cx + y, cy + x, color);
        drawPixel(cx - y, cy + x, color);
        drawPixel(cx + y, cy - x, color);
        drawPixel(cx - y, cy - x, color);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x + 1);
        }
    }
}

void PortraitCanvas::fillCircle(int16_t cx, int16_t cy, int16_t r, uint8_t color) {
    if (r <= 0) return;
    int16_t x = r;
    int16_t y = 0;
    int16_t err = 1 - r;

    while (x >= y) {
        fillRect(cx - x, cy + y, 2 * x + 1, 1, color);
        fillRect(cx - x, cy - y, 2 * x + 1, 1, color);
        fillRect(cx - y, cy + x, 2 * y + 1, 1, color);
        fillRect(cx - y, cy - x, 2 * y + 1, 1, color);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x + 1);
        }
    }
}

void PortraitCanvas::drawCornerArc(int16_t cx, int16_t cy, int16_t r, int quadrant, uint8_t color) {
    if (r <= 0) return;
    int16_t x = r;
    int16_t y = 0;
    int16_t err = 1 - r;
    while (x >= y) {
        int16_t sx1, sy1, sx2, sy2;
        switch (quadrant) {
            case 0:
                sx1 = cx - x; sy1 = cy - y; sx2 = cx - y; sy2 = cy - x; break;
            case 1:
                sx1 = cx + x; sy1 = cy - y; sx2 = cx + y; sy2 = cy - x; break;
            case 2:
                sx1 = cx - x; sy1 = cy + y; sx2 = cx - y; sy2 = cy + x; break;
            default:
                sx1 = cx + x; sy1 = cy + y; sx2 = cx + y; sy2 = cy + x; break;
        }
        drawPixel(sx1, sy1, color);
        drawPixel(sx2, sy2, color);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x + 1);
        }
    }
}

void PortraitCanvas::drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint8_t color) {
    if (w <= 0 || h <= 0 || r < 0) return;
    const int16_t maxr = (w < h ? w : h) / 2;
    if (r > maxr) r = maxr;
    if (r == 0) {
        drawRect(x, y, w, h, color);
        return;
    }

    fillRect(x + r, y, w - 2 * r, 1, color);
    fillRect(x + r, y + h - 1, w - 2 * r, 1, color);
    fillRect(x, y + r, 1, h - 2 * r, color);
    fillRect(x + w - 1, y + r, 1, h - 2 * r, color);

    drawCornerArc(x + r,         y + r,         r, 0, color);
    drawCornerArc(x + w - r - 1, y + r,         r, 1, color);
    drawCornerArc(x + r,         y + h - r - 1, r, 2, color);
    drawCornerArc(x + w - r - 1, y + h - r - 1, r, 3, color);
}

void PortraitCanvas::fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint8_t color) {
    if (w <= 0 || h <= 0 || r < 0) return;
    const int16_t maxr = (w < h ? w : h) / 2;
    if (r > maxr) r = maxr;
    if (r == 0) {
        fillRect(x, y, w, h, color);
        return;
    }

    const int16_t topC = y + r;
    const int16_t botC = y + h - r - 1;
    if (botC >= topC) {
        fillRect(x, topC, w, botC - topC + 1, color);
    }

    const int16_t leftC  = x + r;
    const int16_t rightC = x + w - r - 1;
    const int32_t r2 = (int32_t)r * r;
    for (int16_t dy = 1; dy <= r; dy++) {
        int16_t xext = 0;
        while ((int32_t)(xext + 1) * (xext + 1) + (int32_t)dy * dy <= r2) xext++;
        const int16_t sx = leftC - xext;
        const int16_t sw = (rightC + xext) - sx + 1;
        if (sw > 0) {
            fillRect(sx, topC - dy, sw, 1, color);
            fillRect(sx, botC + dy, sw, 1, color);
        }
    }
}

void PortraitCanvas::fillRoundRectGray(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint8_t gray) {
    if (w <= 0 || h <= 0 || r < 0) return;
    const int16_t maxr = (w < h ? w : h) / 2;
    if (r > maxr) r = maxr;
    if (r == 0) {
        for (int16_t yy = y; yy < y + h; yy++)
            for (int16_t xx = x; xx < x + w; xx++)
                drawPixelGray(xx, yy, gray);
        return;
    }

    const int16_t topC = y + r;
    const int16_t botC = y + h - r - 1;
    const int16_t leftC  = x + r;
    const int16_t rightC = x + w - r - 1;
    const int32_t r2 = (int32_t)r * r;

    if (botC >= topC) {
        for (int16_t yy = topC; yy <= botC; yy++)
            for (int16_t xx = x; xx < x + w; xx++)
                drawPixelGray(xx, yy, gray);
    }

    for (int16_t dy = 1; dy <= r; dy++) {
        int16_t xext = 0;
        while ((int32_t)(xext + 1) * (xext + 1) + (int32_t)dy * dy <= r2) xext++;
        const int16_t sx = leftC - xext;
        const int16_t sw = (rightC + xext) - sx + 1;
        if (sw > 0) {
            for (int16_t xx = sx; xx < sx + sw; xx++) {
                drawPixelGray(xx, topC - dy, gray);
                drawPixelGray(xx, botC + dy, gray);
            }
        }
    }
}

void PortraitCanvas::drawIcon(int16_t x, int16_t y, const uint8_t* icon, int16_t w, int16_t h, uint8_t color) {
    if (!icon || w <= 0 || h <= 0) return;
    const int16_t rowBytes = (w + 7) / 8;
    for (int16_t py = 0; py < h; py++) {
        for (int16_t px = 0; px < w; px++) {
            const uint8_t bit = 0x80 >> (px & 7);
            if (icon[py * rowBytes + (px >> 3)] & bit) {
                drawPixel(x + px, y + py, color);
            }
        }
    }
}

void PortraitCanvas::drawTextCentered(int16_t x, int16_t y, int16_t w, const char* text, int16_t scale, bool bold) {
    if (!text || w <= 0 || scale < 1) return;
    const int16_t tw = FontManager::instance().uiTextWidth(text) * scale;
    int16_t offset = (w - tw) / 2;
    if (offset < 0) offset = 0;
    FontManager::instance().drawText(*this, x + offset, y, text, scale, bold);
}

void PortraitCanvas::drawTextRight(int16_t x, int16_t y, int16_t w, const char* text, int16_t scale, bool bold) {
    if (!text || w <= 0 || scale < 1) return;
    const int16_t tw = FontManager::instance().uiTextWidth(text) * scale;
    int16_t offset = w - tw;
    if (offset < 0) offset = 0;
    FontManager::instance().drawText(*this, x + offset, y, text, scale, bold);
}

void PortraitCanvas::display(RefreshMode mode) {
    display_.display(mode);
}

void PortraitCanvas::requestFullRefresh() {
    display_.requestFullRefresh();
}

void PortraitCanvas::waitRefreshComplete() {
    display_.waitRefreshComplete();
}

}  // namespace eego
