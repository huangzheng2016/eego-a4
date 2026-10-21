// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace eego::dashboard::ui {

inline constexpr size_t kMaxEncodedImageBytes = 512 * 1024;
inline constexpr size_t kMaxImageCacheBytes = 512 * 1024;
inline constexpr size_t kMaxImageCacheEntries = 2;
inline constexpr uint32_t kMaxImageDimension = 4096;
inline constexpr uint32_t kMaxImagePixels = 1024 * 1024;

bool inspectImage(const uint8_t* data, size_t size, uint32_t& width, uint32_t& height);

}  // namespace eego::dashboard::ui
