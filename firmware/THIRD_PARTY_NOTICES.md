# Third-party notices — firmware/

本文件汇总 `firmware/` 固件树直接使用的第三方组件许可与来源。分三类：
**随树打包**（源码在本目录内）、**构建期依赖**（工具链提供，不随树打包）、
**评估后未采用**（记录在案，避免误引入）。

## 随树打包（vendored）

| Component | Version | License | Project |
|---|---:|---|---|
| LodePNG | 20260119 | zlib | <https://github.com/lvandeve/lodepng> |
| Noto Sans CJK SC Bold（数字显示字体位图子集） | 2.004（系统 fonts-noto-cjk） | SIL OFL 1.1 | <https://github.com/notofonts/noto-cjk> |

LodePNG 位于 `components/third_party/lodepng/`，许可文本保留于
`lodepng.cpp` / `lodepng.h` 头部（Copyright (c) 2005-2026 Lode Vandevenne），
不得移除。本副本的裁剪与 ESP-IDF 分配器适配改动已在源码与组件
CMakeLists 中标注（zlib 第 2 条要求的 altered-source 标记）。

`components/eego_ui/display_font_data.cpp` 是由 `tools/gen_digit_font.py` 从
Noto Sans CJK SC Bold（Copyright 2014-2021 Adobe，SIL Open Font License 1.1，
Reserved Font Name "Source"）栅格化的少量字符位图（数字、符号、拉丁字母，四个字号），
随固件分发；OFL 允许嵌入软件分发，位图不作为独立字体出售。重新生成：
`python3 firmware/tools/gen_digit_font.py`（需要 libfreetype 与该字体文件）。

完整的 Noto Sans CJK 字体二进制不随仓库分发；本地转换和嵌入遵循 SIL OFL 1.1，
来源为 <https://github.com/notofonts/noto-cjk>，许可由使用者自行核对。

## 构建期依赖（不随树打包）

| Component | Version | License | Project |
|---|---:|---|---|
| ESP-IDF | v5.5.1 | Apache-2.0 及组件特定条款 | <https://github.com/espressif/esp-idf> |
| Mbed TLS（随 ESP-IDF） | ESP-IDF 内置 | Apache-2.0 | <https://github.com/Mbed-TLS/mbedtls> |
| esptool-js（可选，浏览器 USB 刷机复用项） | 模板仓库 web_flasher 内置 | Apache-2.0 | <https://github.com/espressif/esptool-js> |
| pyserial（仅主机工具 smoke_serial_test.py） | 3.5（requirements.txt 固定） | BSD-3-Clause | <https://github.com/pyserial/pyserial> |

ESP-IDF 及其组件的完整许可以工具链安装副本为准；二进制发布者须保留所用
确切构件的 notices 并履行相应义务。

## 评估后未采用（不随树打包，禁止误引入）

以下来自 weread 参考树的第三方组件经架构评估后**未**进入本固件树
（`tools/validate_firmware.py` 对其中 miniz 等有禁止残留检查）：

| Component | License（上游） | 状态 |
|---|---|---|
| miniz | Unlicense | 未采用；图片解码只用 LodePNG |
| qrcodegen（Nayuki） | MIT | 未打包；配网无二维码渲染需求 |
| HarmonyOS Sans SC | 华为免费商用许可 | 不随仓库分发；本地嵌入由用户自备并核对许可 |
| SdFat / Adafruit GFX / NimBLE-Arduino / arduino-esp32 | MIT / BSD-3 / Apache-2.0 / LGPL-2.1 | 未采用；本树为纯 ESP-IDF，无 Arduino 依赖 |

## 源代码输入（非第三方库）

移植来源的许可边界见 [NOTICE.md](NOTICE.md)：eego-a4-weread（GPL-3.0）
与 eego-a4-template-firmware（MIT）作为源代码输入，衍生作品整体
GPL-3.0-or-later，许可全文见 [LICENSE](LICENSE)。
