// SPDX-License-Identifier: GPL-3.0-or-later
//
//
//
#pragma once

#include "portrait_canvas.h"

#include <stdint.h>
#include <stddef.h>

namespace eego {
class ImageRenderer {
public:
    static ImageRenderer& instance();

    bool renderFromFile(PortraitCanvas& canvas,
                        const char* path,
                        int16_t x, int16_t y,
                        int16_t maxWidth, int16_t maxHeight);

    bool renderFromBuffer(PortraitCanvas& canvas,
                          const uint8_t* data, size_t len,
                          int16_t x, int16_t y,
                          int16_t maxWidth, int16_t maxHeight);

private:
    ImageRenderer() = default;

    bool decodeJpeg(const uint8_t* data, size_t len, uint8_t*& outGray,
                    int* width, int* height, int maxWidth, int maxHeight);

    bool decodePng(const uint8_t* data, size_t len, uint8_t*& outGray,
                   int* width, int* height, int maxWidth, int maxHeight);

    bool decodeGifFirstFrame(const uint8_t* data, size_t len, uint8_t*& outGray,
                             int* width, int* height, int maxWidth, int maxHeight);

    bool drawGrayToCanvas(PortraitCanvas& canvas, const uint8_t* gray,
                          int srcW, int srcH, int16_t x, int16_t y,
                          int16_t maxW, int16_t maxH);
};

}  // namespace eego
