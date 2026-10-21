# SPDX-License-Identifier: GPL-3.0-or-later
# main 组件 — 告警面板骨架（FW1 硬件基础，不含告警业务）
idf_component_register(
    SRCS "app_main.cpp"
    INCLUDE_DIRS "."
    REQUIRES board eego_hal eego_ui
)
