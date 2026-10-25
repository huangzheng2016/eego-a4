# SPDX-License-Identifier: GPL-3.0-or-later
# Host build of the real dashboard_ui renderer + eego_ui canvas/fonts with
# host stubs for the display/heap/SD (library `dashboard_ui_host`, always
# built: the ui unit tests link it). The PNG render harness `dashboard_render`
# is optional (-DEEGO_HOST_RENDER=ON); see render.sh for the one-shot command.
set(R ${CMAKE_CURRENT_SOURCE_DIR})
add_library(dashboard_ui_host STATIC
  ${R}/host_font.cpp
  ${C}/dashboard_ui/src/text.cpp
  ${C}/dashboard_ui/src/panel_style.cpp
  ${C}/dashboard_ui/src/panels.cpp
  ${C}/dashboard_ui/src/renderer.cpp
  ${C}/dashboard_ui/src/update.cpp
  ${C}/dashboard_ui/src/image.cpp
  ${C}/dashboard_ui/src/image_probe.cpp
  ${C}/dashboard_ui/src/preview_render.cpp
  ${C}/eego_ui/portrait_canvas.cpp
  ${C}/eego_ui/fallback_font.cpp
  ${C}/eego_ui/display_font.cpp
  ${C}/eego_ui/display_font_data.cpp
  ${C}/eego_ui/image_renderer.cpp
  ${C}/third_party/lodepng/lodepng.cpp
)
# Stubs first so the host display/heap/SD replacements shadow the ESP-IDF ones.
target_include_directories(dashboard_ui_host BEFORE PUBLIC ${R}/stubs)
target_include_directories(dashboard_ui_host PUBLIC
  ${R}
  ${C}/eego_ui/include
  ${C}/eego_ui
  ${C}/eego_hal/include
  ${C}/third_party/lodepng
  ${C}/dashboard_ui/src
)
target_compile_options(dashboard_ui_host PRIVATE ${HT_WARN} ${HT_SAN})
target_link_libraries(dashboard_ui_host PUBLIC fw_under_test)
find_package(Freetype QUIET)
if(FREETYPE_FOUND)
  target_compile_definitions(dashboard_ui_host PRIVATE EEGO_RENDER_FREETYPE=1)
  target_include_directories(dashboard_ui_host PRIVATE ${FREETYPE_INCLUDE_DIRS})
  target_link_libraries(dashboard_ui_host PUBLIC ${FREETYPE_LIBRARIES})
endif()

if(EEGO_HOST_RENDER)
  add_executable(dashboard_render ${R}/render_main.cpp)
  target_compile_options(dashboard_render PRIVATE ${HT_WARN} ${HT_SAN})
  target_link_libraries(dashboard_render PRIVATE dashboard_ui_host)
endif()
