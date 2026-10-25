// SPDX-License-Identifier: GPL-3.0-or-later
// Host render-harness stub of the UC8279C driver: an in-memory 4-bit gray
// framebuffer (0=black .. 15=white) in logical portrait coordinates. B/W
// drawing stores 0/15; drawPixelGray stores the level so the harness can emit
// both the full-refresh (4-level) and fast-refresh (thresholded) views.
#pragma once

#include <stdint.h>

#include "eego_pins.h"
#include "esp_err.h"

namespace eego {

enum class RefreshMode { Fast, Full };

class Uc8279cDisplay {
public:
    static constexpr uint16_t WIDTH = LOGICAL_WIDTH;
    static constexpr uint16_t HEIGHT = LOGICAL_HEIGHT;
    Uc8279cDisplay() { clear(); }

    esp_err_t begin() { return ESP_OK; }
    esp_err_t beginOffscreen() { clear(); return ESP_OK; }
    bool offscreen() const { return true; }
    void end() {}

    void clear() {
        for (auto& px : gray_) px = 15;
    }
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color) {
        for (int16_t yy = y; yy < y + h; ++yy)
            for (int16_t xx = x; xx < x + w; ++xx) drawPixel(xx, yy, color);
    }
    void drawPixel(int16_t x, int16_t y, uint8_t color) {
        if (x < 0 || y < 0 || x >= LOGICAL_WIDTH || y >= LOGICAL_HEIGHT) return;
        gray_[y * LOGICAL_WIDTH + x] = color ? 15 : 0;
    }
    uint8_t getPixel(int16_t x, int16_t y) const { return grayAt(x, y) >= 8 ? 1 : 0; }
    void drawPixelGray(int16_t x, int16_t y, uint8_t gray) {
        if (x < 0 || y < 0 || x >= LOGICAL_WIDTH || y >= LOGICAL_HEIGHT) return;
        gray_[y * LOGICAL_WIDTH + x] = gray > 15 ? 15 : gray;
    }
    void clearGray() {}
    bool hasGrayContent() const { return true; }
    static uint8_t grayToBw(uint8_t gray) { return gray >= 8 ? 1 : 0; }
    uint8_t grayLevel(int16_t x, int16_t y) const { return static_cast<uint8_t>(grayAt(x, y) >> 2); }

    void display(RefreshMode = RefreshMode::Fast, bool = false) {}
    void waitRefreshComplete() {}
    void displayGray(bool = false) {}
    void requestFullRefresh() {}

    uint8_t grayAt(int16_t x, int16_t y) const {
        if (x < 0 || y < 0 || x >= LOGICAL_WIDTH || y >= LOGICAL_HEIGHT) return 15;
        return gray_[y * LOGICAL_WIDTH + x];
    }

private:
    uint8_t gray_[LOGICAL_WIDTH * LOGICAL_HEIGHT];
};

}  // namespace eego
