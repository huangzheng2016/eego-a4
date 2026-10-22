# SPDX-License-Identifier: GPL-3.0-or-later
# main 组件 — 告警面板组合根 + app_task（FW5/M18 + FW5B/M21 接线）
#
# app_main.cpp     组合根与 app_task 主循环（唯一 UI 属主）
# dashboard_runtime.* dashboard 配置/缓存/调度/导航/动作/局部渲染（DP4/M26）
# panel_app.*      应用状态机（页面/导航/动作/电源 FSM）
# panel_render.*   S0–S4 纯绘制层（安全区内，灰阶严重度）
# panel_diag.cpp   diag 串口命令钩子实现
# ui_text.*        中英界面文案表
# device_prefs.*   旧 NVS "ui" 静音指纹的一次性迁移（其他偏好在 device_settings）
# time_source.*    统一时间源（SNTP 系统时间 > RTC > 不可用；M21）
# input_merge.*    触控/实体键同批时间戳归并（DP8 R1）
idf_component_register(
    SRCS
        "app_main.cpp"
        "dashboard_runtime.cpp"
        "panel_app.cpp"
        "panel_diag.cpp"
        "panel_render.cpp"
        "ui_text.cpp"
        "device_prefs.cpp"
        "time_source.cpp"
        "input_confirm.cpp"
        "input_navigation.cpp"
        "input_merge.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        board
        eego_hal
        eego_ui
        admin
        alert_core
        ble
        dashboard_core
        dashboard_ui
        data_pipeline
        device_settings
        net_stack
        diag
        nvs_flash
        esp_timer
        esp_system
        esp_app_format
        esp_hw_support
        spi_flash
        heap
        driver
        freertos
)
