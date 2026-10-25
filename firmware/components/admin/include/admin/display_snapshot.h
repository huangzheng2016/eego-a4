// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// GET /api/v1/display/snapshot.bmp —— 当前墨水屏帧（portrait 552×768，1-bpp BMP）。
// admin 不依赖 UI/显示驱动：由 app 侧注册一个 provider，把当前帧按 portrait
// 方向打包成 1-bpp 行写入 admin 在 PSRAM 中分配的缓冲区。

#include <cstddef>
#include <cstdint>
#include <functional>

namespace eego::admin {

inline constexpr uint32_t kSnapshotWidth = 552;
inline constexpr uint32_t kSnapshotHeight = 768;
// BMP rows are padded to 4 bytes: 552 px = 69 bytes -> 72.
inline constexpr size_t kSnapshotStride = ((kSnapshotWidth + 31) / 32) * 4;
inline constexpr size_t kSnapshotHeaderBytes = 14 + 40 + 8;  // file + info header + 2-color palette
inline constexpr size_t kSnapshotFileBytes = kSnapshotHeaderBytes + kSnapshotStride * kSnapshotHeight;

// Fill `rows`: kSnapshotHeight portrait rows, top row first, each `stride`
// bytes; pixel (x, y) is bit (7 - x % 8) of rows[y * stride + x / 8], 1 = white,
// 0 = black (same polarity as Uc8279cDisplay). The buffer is zeroed (black)
// before the call. Runs on the httpd task: the provider must synchronize with
// the renderer itself. Return false when no frame is available (-> 503).
using DisplaySnapshotProvider = std::function<bool(uint8_t* rows, size_t stride)>;

// Register (or clear with nullptr) the provider. Thread-safe; may be called
// before or after startAdminApi().
void setDisplaySnapshotProvider(DisplaySnapshotProvider provider);

// Writes the 62-byte header of a top-down (negative height) 1-bpp BMP with a
// black(0)/white(1) palette for a kSnapshotWidth x kSnapshotHeight image.
inline void writeSnapshotBmpHeader(uint8_t* out) {
  auto le16 = [&](size_t at, uint32_t v) {
    out[at] = static_cast<uint8_t>(v);
    out[at + 1] = static_cast<uint8_t>(v >> 8);
  };
  auto le32 = [&](size_t at, uint32_t v) {
    for (size_t i = 0; i < 4; ++i) out[at + i] = static_cast<uint8_t>(v >> (8 * i));
  };
  for (size_t i = 0; i < kSnapshotHeaderBytes; ++i) out[i] = 0;
  out[0] = 'B';
  out[1] = 'M';
  le32(2, static_cast<uint32_t>(kSnapshotFileBytes));
  le32(10, static_cast<uint32_t>(kSnapshotHeaderBytes));          // pixel data offset
  le32(14, 40);                                                    // BITMAPINFOHEADER
  le32(18, kSnapshotWidth);
  le32(22, static_cast<uint32_t>(-static_cast<int32_t>(kSnapshotHeight)));  // top-down
  le16(26, 1);                                                     // planes
  le16(28, 1);                                                     // 1 bpp
  le32(34, static_cast<uint32_t>(kSnapshotStride * kSnapshotHeight));
  le32(38, 3780);                                                  // 96 dpi
  le32(42, 3780);
  le32(46, 2);                                                     // palette entries
  le32(50, 2);
  // Palette (B, G, R, 0): index 0 = black, index 1 = white.
  out[58] = out[59] = out[60] = 0xFF;
}

}  // namespace eego::admin
