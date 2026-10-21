// SPDX-License-Identifier: GPL-3.0-or-later
// fallback_font.h — 内置 5x7 ASCII 回退字体
//
// SD 卡字体（.eefont / .bin）缺席时保证面板仍可显示文字：
// 覆盖大写字母 / 数字 / 常用标点，小写映射为大写，其余字符画 .notdef 占位框。
// 字形为通用 5x7 点阵（本工程自制，无第三方品牌资源）。
// 渲染约定与 CpFontRenderer 一致：gray==0 走纯黑 drawPixel（BW 帧），
// gray>0 走 drawPixelGray（四级灰阶帧）。
#pragma once

#include <stdint.h>

#include "portrait_canvas.h"

namespace eego {

// 5x7 点阵尺寸（scale=1 时）
constexpr int16_t FALLBACK_GLYPH_W = 5;
constexpr int16_t FALLBACK_GLYPH_H = 7;
constexpr int16_t FALLBACK_ADVANCE = 6;  // 字宽 5 + 间距 1

// 测量一行 UTF-8 文本的像素宽度（scale 倍）
int16_t fallbackTextWidth(const char* utf8, int16_t scale);

// 在 (x, y) 绘制文本：y 为字形 cell 顶部，scale 整数放大。
// gray==0 纯黑；gray>0 统一灰度（0-15）。返回结束 x 坐标。
int16_t fallbackDrawText(PortraitCanvas& canvas, int16_t x, int16_t y,
                         const char* utf8, int16_t scale, uint8_t gray);

}  // namespace eego
