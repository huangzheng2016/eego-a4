# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(
  SRCS
    "src/geometry.cpp"
    "src/layout.cpp"
    "src/update.cpp"
    "src/text.cpp"
    "src/image_probe.cpp"
    "src/image.cpp"
    "src/panel_style.cpp"
    "src/panels.cpp"
    "src/renderer.cpp"
    "src/preview_data.cpp"
    "src/preview_render.cpp"
  INCLUDE_DIRS "include"
  REQUIRES dashboard_core eego_ui eego_hal
  PRIV_REQUIRES heap esp_timer
)
