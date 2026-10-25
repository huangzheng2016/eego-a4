# SPDX-License-Identifier: GPL-3.0-or-later
# eego_ui 组件 — 画布 + 字体渲染 + 图片解码（灰阶）
# (PortraitCanvas / EDCBook .bin / CPFONT v4 .eefont / 字体管理器 /
#  ImageRenderer / 内置 5x7 回退字体)
#
# 移植自 eego-a4-weread（GPL-3.0）components/eego_ui。本组件为 GPL-3.0 衍生作品。
# 相对参考实现的删减：ImageRenderer 移除 HTTPS 下载 / MD5 缓存（阅读器封面缓存体系），
# 仅保留解码与灰阶上屏 —— 网络访问由 net_task 独占 esp_http_client，UI 层不做 HTTP。
idf_component_register(
    SRCS
        "eego_ui.cpp"
        "portrait_canvas.cpp"
        "fallback_font.cpp"
        "edcbook_font.cpp"
        "cpfont_renderer.cpp"
        "font_manager.cpp"
        "image_renderer.cpp"
    INCLUDE_DIRS "include"
    # esp_rom: ESP32-S3 ROM tjpgd (jd_prepare/jd_decomp) 头文件与符号
    # lodepng: PNG 解码（zlib 许可，third_party 组件）
    REQUIRES eego_hal lodepng esp_rom
    PRIV_REQUIRES heap
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fonts/HarmonyOS_Sans_SC_12.eefont")
    target_add_binary_data(${COMPONENT_LIB} "fonts/HarmonyOS_Sans_SC_12.eefont" "BINARY")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE EEGO_EMBEDDED_FONT=1)
endif()
