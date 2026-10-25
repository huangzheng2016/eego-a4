// SPDX-License-Identifier: GPL-3.0-or-later
// display_font.h — 内置原生尺寸数字显示字体（大数值专用）
//
// 由 tools/gen_digit_font.py 从 Noto Sans CJK SC Bold（SIL OFL 1.1）生成的少量
// 字符（数字、符号、拉丁字母）在四个字号下的 2-bit 位图（display_font_data.cpp）。
// 相比把 UI 字体整数放大，大字号数值边缘平滑、笔画比例正确。
// 绘制：覆盖率 3 级画黑，2 级画深灰、1 级画浅灰（drawPixelGray）——Fast 刷新按
// 灰度阈值得到约 50% 覆盖率的二值字形，Full 刷新显示抗锯齿边缘。
// 字符集外的字符回退到 UI 字体（FontManager，按字号整数放大并对齐基线）。
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "portrait_canvas.h"

namespace eego::display_font {

// 字号数（由小到大，索引 0..count()-1）
size_t count();
int16_t capHeight(size_t size);
int16_t ascent(size_t size);      // 行顶部 -> 基线
int16_t lineHeight(size_t size);  // ascent + descent
// 文本是否全部在内置字符集内（否则部分字符走 UI 字体回退）
bool covers(size_t size, const char* utf8);
// 像素宽度（含回退字符）
int16_t textWidth(size_t size, const char* utf8);
// 在 (x, top) 绘制：基线 = top + ascent(size)。ink 0=黑字，15=白字（反色底，
// 不画灰阶边缘）。返回结束 x。
int16_t drawText(PortraitCanvas& canvas, size_t size, int16_t x, int16_t top,
                 const char* utf8, uint8_t ink = 0);

}  // namespace eego::display_font
