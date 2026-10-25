# Third-party notices — firmware/

本文件汇总 `firmware/` 固件树直接使用的第三方组件许可与来源。分三类：
**随树打包**（源码在本目录内）、**构建期依赖**（工具链提供，不随树打包）、
**评估后未采用**（记录在案，避免误引入）。

## 随树打包（vendored）

| Component | Version | License | Project |
|---|---:|---|---|
| LodePNG | 20260119 | zlib | <https://github.com/lvandeve/lodepng> |

LodePNG 位于 `components/third_party/lodepng/`，许可文本保留于
`lodepng.cpp` / `lodepng.h` 头部（Copyright (c) 2005-2026 Lode Vandevenne），
不得移除。本副本的裁剪与 ESP-IDF 分配器适配改动已在源码与组件
CMakeLists 中标注（zlib 第 2 条要求的 altered-source 标记）。

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
