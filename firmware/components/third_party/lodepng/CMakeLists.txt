# SPDX-License-Identifier: GPL-3.0-or-later
# lodepng — 单文件 PNG 解码库（zlib 许可，Copyright (c) 2005-2026 Lode Vandevenne）
#
# 上游: https://github.com/lvandeve/lodepng （版本 20260119）
# 本副本改动（均已在源码中标注）：
#   1. 裁剪：LODEPNG_NO_COMPILE_DISK / LODEPNG_NO_COMPILE_ANCILLARY_CHUNKS
#      （嵌入式无文件系统 IO，只需像素数据）
#   2. 分配器改走 ESP-IDF heap_caps（优先 PSRAM，回退内部 RAM），
#      大图解码缓冲不再挤占内部 RAM
# 许可声明保留于 lodepng.cpp / lodepng.h 头部，不得移除。
idf_component_register(
    SRCS "lodepng.cpp"
    INCLUDE_DIRS "."
    REQUIRES heap
)
