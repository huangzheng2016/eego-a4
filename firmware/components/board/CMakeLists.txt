# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(
    SRCS "board_boot.cpp"
    INCLUDE_DIRS "include"
    REQUIRES eego_hal
)
