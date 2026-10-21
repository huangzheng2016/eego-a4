# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(
    SRCS
        "src/settings_model.cpp"
        "src/device_settings.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        alert_core
        nvs_flash
        freertos
)
