# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(
    SRCS
        "src/observer_core.cpp"
        "src/ble_manager.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        alert_core
        device_settings
        net_stack
        bt
        wifi_provisioning
        esp_wifi
        esp_event
        esp_timer
        nvs_flash
        freertos
)
