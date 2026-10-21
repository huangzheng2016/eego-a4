# SPDX-License-Identifier: GPL-3.0-or-later
# eego_hal 组件 — EEGO A4 硬件抽象层
# 显示 UC8279C / 触控 GSLX680 / SD HSPI / 电源电池RTC / 前光 / 按键
#
# 移植自 eego-a4-weread（GPL-3.0）components/eego_hal，板级事实来源：
# eego-a4-template-firmware（MIT）。本组件为 GPL-3.0 衍生作品。
idf_component_register(
    SRCS
        "eego_hal.cpp"
        "uc8279c_display.cpp"
        "gslx680_touch.cpp"
        "sd_card.cpp"
        "power_manager.cpp"
        "frontlight.cpp"
        "pcf8563_rtc.cpp"
        "buttons.cpp"
        "input_gesture.cpp"
    INCLUDE_DIRS "include"
    # 公共头文件 (uc8279c_display.h / power_manager.h / pcf8563_rtc.h /
    # gslx680_touch.h / sd_card.h) 直接 include driver/* 与 esp_adc/、sdmmc_cmd.h，
    # 必须放在 REQUIRES 以便外部组件 (eego_ui) 解析这些头。
    # driver (legacy) 提供 driver/i2c.h（legacy I2C API），并传递依赖
    # esp_driver_spi/gpio/i2c 等。
    REQUIRES
        driver
        sdmmc
        esp_adc
    PRIV_REQUIRES
        fatfs
        esp_timer
        esp_psram
        heap
)
