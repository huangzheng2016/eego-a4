// SPDX-License-Identifier: GPL-3.0-or-later
//
#pragma once

#include <stdint.h>

#include "portrait_canvas.h"

namespace eego {
constexpr int16_t FALLBACK_GLYPH_W = 5;
constexpr int16_t FALLBACK_GLYPH_H = 7;
constexpr int16_t FALLBACK_ADVANCE = 6;

int16_t fallbackTextWidth(const char* utf8, int16_t scale);

int16_t fallbackDrawText(PortraitCanvas& canvas, int16_t x, int16_t y,
                         const char* utf8, int16_t scale, uint8_t gray);

}  // namespace eego
