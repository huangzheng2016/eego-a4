# SPDX-License-Identifier: GPL-3.0-or-later
# board 组件 — EEGO A4 板级启动事实（启动顺序合同，见 board_boot.h）
idf_component_register(
    SRCS "board_boot.cpp"
    INCLUDE_DIRS "include"
    REQUIRES eego_hal
)
