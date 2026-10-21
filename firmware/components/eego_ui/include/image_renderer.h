// SPDX-License-Identifier: GPL-3.0-or-later
// 移植自 eego-a4-weread（GPL-3.0）同名文件；原始硬件事实来自 eego-a4-template-firmware（MIT）。
// ImageRenderer — 图片解码 / 灰度双线性缩放上屏
// 纯 ESP-IDF，无 Arduino 依赖。
//
// 相对参考实现的删减：移除 HTTPS 下载 / MD5 URL 缓存 / 缩略图缓存（阅读器封面缓存体系），
// 仅保留解码与灰阶上屏能力。网络访问由 net_task 独占 esp_http_client，UI 层不做 HTTP。
//
// 解码器选择：
//   JPEG — ESP32-S3 ROM 内置 tjpgd（jd_prepare / jd_decomp，RGB888 输出）
//   PNG  — lodepng（third_party 组件，zlib 许可；lodepng_decode32 → RGBA → BT.601 灰度）
//   GIF  — 占位（项目无 AnimatedGIF，返回 false）
//
// 注意：
//   - decodeJpeg/decodePng/decodeGifFirstFrame 的 outGray 为输出参数（引用）：
//     解码尺寸在解析文件头后才确定，由解码器内部分配 PSRAM 缓冲并返回，
//     调用方负责 heap_caps_free。
//   - 图片缩放使用双线性插值。
#pragma once

#include "portrait_canvas.h"

#include <stdint.h>
#include <stddef.h>

namespace eego {

class ImageRenderer {
public:
    static ImageRenderer& instance();

    // 从 SD 卡文件加载图片并渲染到画布。
    // 支持 JPEG / PNG（GIF 暂未实现返回 false）。
    // path 为相对 /sdcard 的路径（如 "icons/alert.png"）。
    // 自动按 (maxWidth × maxHeight) 等比缩放（双线性），灰度抖动上屏。
    bool renderFromFile(PortraitCanvas& canvas,
                        const char* path,
                        int16_t x, int16_t y,
                        int16_t maxWidth, int16_t maxHeight);

    // 从内存缓冲区渲染（magic bytes 判定格式）。
    bool renderFromBuffer(PortraitCanvas& canvas,
                          const uint8_t* data, size_t len,
                          int16_t x, int16_t y,
                          int16_t maxWidth, int16_t maxHeight);

private:
    ImageRenderer() = default;

    // 解码 JPEG 到灰度缓冲（0-255，BT.601 亮度）。
    // outGray 为输出参数：解码器内部分配 PSRAM 缓冲，调用方负责释放。
    // width/height 输出解码后尺寸（按 maxWidth/maxHeight 选 tjpgd 缩放档）。
    bool decodeJpeg(const uint8_t* data, size_t len, uint8_t*& outGray,
                    int* width, int* height, int maxWidth, int maxHeight);

    // PNG 解码到灰度缓冲（0-255，BT.601 亮度）。
    // 用 lodepng_decode32 解码为 RGBA，再转灰度；outGray 为 PSRAM 缓冲，调用方负责释放。
    // lodepng 无缩放档，整图解码；超大图按 kMaxDecodedPixels 拦截。
    bool decodePng(const uint8_t* data, size_t len, uint8_t*& outGray,
                   int* width, int* height, int maxWidth, int maxHeight);

    // GIF 首帧解码（暂未实现，返回 false）。
    bool decodeGifFirstFrame(const uint8_t* data, size_t len, uint8_t*& outGray,
                             int* width, int* height, int maxWidth, int maxHeight);

    // 灰度缓冲（0-255）双线性缩放到目标区域并上屏（0-15 + Bayer 抖动）。
    // 等比适配进 (maxW × maxH)，居中。
    void drawGrayToCanvas(PortraitCanvas& canvas, const uint8_t* gray,
                          int srcW, int srcH, int16_t x, int16_t y,
                          int16_t maxW, int16_t maxH);
};

}  // namespace eego
