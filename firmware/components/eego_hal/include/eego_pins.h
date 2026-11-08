// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

namespace eego {
constexpr uint32_t FLASH_BYTES = 16U * 1024U * 1024U;
constexpr uint32_t PSRAM_BYTES = 8U * 1024U * 1024U;

constexpr uint16_t PANEL_WIDTH = 768;
constexpr uint16_t PANEL_HEIGHT = 552;
constexpr uint16_t CONTROLLER_HEIGHT = 600;
constexpr uint16_t CONTROLLER_PADDING_ROWS = 48;
constexpr uint16_t LOGICAL_WIDTH = PANEL_HEIGHT;
constexpr uint16_t LOGICAL_HEIGHT = PANEL_WIDTH;
constexpr uint16_t PANEL_ROW_BYTES = PANEL_WIDTH / 8;  // 96
constexpr uint32_t FRAMEBUFFER_SIZE = static_cast<uint32_t>(PANEL_ROW_BYTES) * PANEL_HEIGHT;  // 52992

constexpr int16_t DISPLAY_OUTER_RADIUS = 60;
constexpr int16_t SAFE_CONTENT_INSET = 12;
constexpr int16_t SAFE_CONTENT_RADIUS = DISPLAY_OUTER_RADIUS - SAFE_CONTENT_INSET;  // 48
constexpr int16_t SAFE_CONTENT_X = SAFE_CONTENT_INSET;
constexpr int16_t SAFE_CONTENT_Y = SAFE_CONTENT_INSET;
constexpr int16_t SAFE_CONTENT_WIDTH = LOGICAL_WIDTH - SAFE_CONTENT_INSET * 2;   // 528
constexpr int16_t SAFE_CONTENT_HEIGHT = LOGICAL_HEIGHT - SAFE_CONTENT_INSET * 2; // 744
constexpr int16_t SAFE_CONTENT_RIGHT = SAFE_CONTENT_X + SAFE_CONTENT_WIDTH - 1;
constexpr int16_t SAFE_CONTENT_BOTTOM = SAFE_CONTENT_Y + SAFE_CONTENT_HEIGHT - 1;

constexpr int16_t UI_CONTENT_INSET = 28;
constexpr int16_t UI_CONTENT_X = UI_CONTENT_INSET;
constexpr int16_t UI_CONTENT_Y = UI_CONTENT_INSET;
constexpr int16_t UI_CONTENT_WIDTH = LOGICAL_WIDTH - UI_CONTENT_INSET * 2;   // 496
constexpr int16_t UI_CONTENT_HEIGHT = LOGICAL_HEIGHT - UI_CONTENT_INSET * 2; // 712
constexpr int16_t UI_CONTENT_RIGHT = UI_CONTENT_X + UI_CONTENT_WIDTH - 1;
constexpr int16_t UI_CONTENT_BOTTOM = UI_CONTENT_Y + UI_CONTENT_HEIGHT - 1;

constexpr int8_t EPD_SCLK = 42;
constexpr int8_t EPD_MOSI = 45;
constexpr int8_t EPD_CS = 21;
constexpr int8_t EPD_DC = 14;
constexpr int8_t EPD_RESET = 13;
constexpr int8_t EPD_BUSY = 41;
constexpr int8_t EPD_POWER_ENABLE = 6;
constexpr uint32_t EPD_SPI_HZ = 20000000;

constexpr int8_t SD_SCLK = 39;
constexpr int8_t SD_MISO = 40;
constexpr int8_t SD_MOSI = 38;
constexpr int8_t SD_CS = 47;
constexpr uint32_t SD_SPI_HZ = 20000000;

constexpr int8_t I2C_SDA = 2;
constexpr int8_t I2C_SCL = 1;
constexpr uint32_t I2C_HZ = 400000;
constexpr uint8_t GSLX680_ADDRESS = 0x40;
constexpr uint8_t PCF8563_ADDRESS = 0x51;
constexpr uint8_t LM3630A_ADDRESS = 0x36;

constexpr int8_t BUTTON_UP = 5;       // active-low
constexpr int8_t BUTTON_DOWN = 7;     // active-low
constexpr int8_t BUTTON_POWER = 8;    // active-high
constexpr uint16_t FRONT_KEY_RAW_X = 0x03a0;
constexpr uint16_t FRONT_KEY_RAW_Y = 0x1020;
constexpr uint32_t FRONT_KEY_LONG_MS = 700;

constexpr int8_t BATTERY_ADC = 10;
constexpr float BATTERY_DIVIDER = 1.559f;
constexpr int8_t CHARGE_STATUS = 11;

constexpr int8_t FRONTLIGHT_ENABLE = 12;

constexpr int8_t POWER_LATCH = 4;

inline uint16_t rawTouchToLogicalX(uint16_t rawY) {
    uint16_t limitedY = rawY > 680 ? 680 : rawY;
    return static_cast<uint32_t>(limitedY) * 551 / 680;
}

inline uint16_t rawTouchToLogicalY(uint16_t rawX) {
    uint16_t limitedX = rawX > 920 ? 920 : rawX;
    return static_cast<uint32_t>(920 - limitedX) * 767 / 920;
}

}  // namespace eego
