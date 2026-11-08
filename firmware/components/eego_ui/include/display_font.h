// SPDX-License-Identifier: GPL-3.0-or-later
//
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "portrait_canvas.h"

namespace eego::display_font {
size_t count();
int16_t capHeight(size_t size);
int16_t ascent(size_t size);
int16_t lineHeight(size_t size);  // ascent + descent
bool covers(size_t size, const char* utf8);
int16_t textWidth(size_t size, const char* utf8);
int16_t drawText(PortraitCanvas& canvas, size_t size, int16_t x, int16_t top,
                 const char* utf8, uint8_t ink = 0);

}  // namespace eego::display_font
