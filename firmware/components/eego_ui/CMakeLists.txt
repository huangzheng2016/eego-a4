# SPDX-License-Identifier: GPL-3.0-or-later
#
idf_component_register(
    SRCS
        "eego_ui.cpp"
        "portrait_canvas.cpp"
        "fallback_font.cpp"
        "display_font.cpp"
        "display_font_data.cpp"
        "edcbook_font.cpp"
        "cpfont_renderer.cpp"
        "font_manager.cpp"
        "image_renderer.cpp"
    INCLUDE_DIRS "include"
    REQUIRES eego_hal lodepng esp_rom
    PRIV_REQUIRES heap
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fonts/NotoSansCJKsc_6.cpfont")
    target_add_binary_data(${COMPONENT_LIB} "fonts/NotoSansCJKsc_6.cpfont" "BINARY")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE EEGO_EMBEDDED_FONT=1)
endif()
