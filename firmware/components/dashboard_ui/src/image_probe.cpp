// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstddef>
#include <cstdint>

#include "dashboard_ui/image_probe.h"

namespace eego::dashboard::ui {
namespace {

uint16_t be16(const uint8_t* data) {
  return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

uint32_t be32(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24) |
         (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) | data[3];
}

bool bounded(uint32_t width, uint32_t height) {
  return width > 0 && height > 0 && width <= kMaxImageDimension &&
         height <= kMaxImageDimension &&
         static_cast<uint64_t>(width) * height <= kMaxImagePixels;
}

bool jpegStartOfFrame(uint8_t marker) {
  switch (marker) {
    case 0xc0:
    case 0xc1:
    case 0xc2:
    case 0xc3:
    case 0xc5:
    case 0xc6:
    case 0xc7:
    case 0xc9:
    case 0xca:
    case 0xcb:
    case 0xcd:
    case 0xce:
    case 0xcf:
      return true;
    default:
      return false;
  }
}

}  // namespace

bool inspectImage(const uint8_t* data, size_t size, uint32_t& width, uint32_t& height) {
  width = 0;
  height = 0;
  if (data == nullptr || size < 4 || size > kMaxEncodedImageBytes) return false;
  static constexpr uint8_t kPngSignature[8] = {0x89, 0x50, 0x4e, 0x47,
                                               0x0d, 0x0a, 0x1a, 0x0a};
  if (size >= 24) {
    bool png = true;
    for (size_t i = 0; i < 8; ++i) png = png && data[i] == kPngSignature[i];
    if (png) {
      if (be32(data + 8) != 13 || data[12] != 'I' || data[13] != 'H' ||
          data[14] != 'D' || data[15] != 'R') {
        return false;
      }
      width = be32(data + 16);
      height = be32(data + 20);
      if (!bounded(width, height)) {
        width = 0;
        height = 0;
        return false;
      }
      return true;
    }
  }

  if (data[0] != 0xff || data[1] != 0xd8) return false;
  size_t position = 2;
  while (position + 1 < size) {
    while (position < size && data[position] != 0xff) ++position;
    while (position < size && data[position] == 0xff) ++position;
    if (position >= size) break;
    const uint8_t marker = data[position++];
    if (marker == 0xd9 || marker == 0xda) break;
    if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
    if (position + 2 > size) break;
    const uint16_t length = be16(data + position);
    if (length < 2 || position + length > size) return false;
    if (jpegStartOfFrame(marker)) {
      if (length < 7) return false;
      height = be16(data + position + 3);
      width = be16(data + position + 5);
      if (!bounded(width, height)) {
        width = 0;
        height = 0;
        return false;
      }
      return true;
    }
    position += length;
  }
  width = 0;
  height = 0;
  return false;
}

}  // namespace eego::dashboard::ui
