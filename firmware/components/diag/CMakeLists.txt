# SPDX-License-Identifier: GPL-3.0-or-later
#
idf_component_register(
    SRCS "src/diag.cpp"
    INCLUDE_DIRS "include"
    REQUIRES freertos log
)
