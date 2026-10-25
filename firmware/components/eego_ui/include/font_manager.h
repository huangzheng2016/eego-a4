// SPDX-License-Identifier: GPL-3.0-or-later
// 移植自 eego-a4-weread（GPL-3.0）同名文件；原始硬件事实来自 eego-a4-template-firmware（MIT）。
// font_manager.h — 字体管理器
//
// 统一管理 UI 字体（固定，启动加载）与正文字体（可切换），自动识别格式：
//   - 读文件前 8 字节 magic："EEFONT" / "CPFONT" → CPFONT v4（.eefont）
//   - 其他 → EDCBook .bin
// 渲染通过 PortraitCanvas 完成。单例。
// 未加载任何 SD 字体时，渲染/测量 API 自动回退到内置 5x7 ASCII 字体
// （fallback_font.h），保证面板始终可显示文字。
#pragma once

#include <stdint.h>

#include "portrait_canvas.h"

namespace eego {

class EdcFont;
class CpFontRenderer;

enum class FontFormat { Unknown, EdcbookBin, CpFontV4 };

class FontManager {
public:
    static FontManager& instance();

    // 加载 UI 字体（固定，启动加载）
    bool loadUiFont(const char* path);
    // 加载构建时嵌入的本地 UI 字体；无嵌入文件时返回 false
    bool loadUiFontEmbedded();
    // 加载正文字体（可切换）
    bool loadBodyFont(const char* path);

    // 自动识别格式（读前 8 字节 magic）
    static FontFormat detectFormat(const char* path);

    // 渲染 API（y 为文本顶部）
    void drawText(PortraitCanvas& canvas, int16_t x, int16_t y,
                  const char* utf8, int16_t scale = 1, bool bold = false);
    void drawTextBody(PortraitCanvas& canvas, int16_t x, int16_t y,
                      const char* utf8, int16_t scale = 1, bool bold = false);
    // 以统一灰度 gray(0-15)绘制文本（UI 字体），用于深灰辅助文字
    void drawTextGray(PortraitCanvas& canvas, int16_t x, int16_t y,
                      const char* utf8, int16_t scale, uint8_t gray);

    // 字体信息
    int16_t uiFontHeight() const;
    // UI 字体基线距文本顶部的像素数（scale=1）。用于不同字号文字的基线对齐。
    int16_t uiFontAscender() const;
    int16_t bodyFontHeight() const;
    int16_t uiTextWidth(const char* utf8) const;
    int16_t bodyTextWidth(const char* utf8) const;

    bool uiLoaded() const;
    bool bodyLoaded() const;
    FontFormat uiFormat() const { return uiFmt_; }
    FontFormat bodyFormat() const { return bodyFmt_; }

private:
    FontManager() = default;
    ~FontManager();
    FontManager(const FontManager&) = delete;
    FontManager& operator=(const FontManager&) = delete;

    void unloadUi();
    void unloadBody();
    bool loadInto(const char* path, FontFormat& fmtOut,
                  EdcFont*& edcOut, CpFontRenderer*& cpOut);

    EdcFont*        uiEdc_ = nullptr;
    CpFontRenderer* uiCp_  = nullptr;
    FontFormat      uiFmt_ = FontFormat::Unknown;

    EdcFont*        bodyEdc_ = nullptr;
    CpFontRenderer* bodyCp_  = nullptr;
    FontFormat      bodyFmt_ = FontFormat::Unknown;
};

}  // namespace eego
