# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(
    SRCS
        "src/observer_core.cpp"
        "src/ble_manager.cpp"
        "src/tunnel_frame.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        alert_core
        device_settings
        net_stack
        bt
        esp_hw_support
        heap
        esp_event
        esp_timer
        nvs_flash
        freertos
)
