// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "eego_pins.h"
#include <esp_err.h>
#include <stdint.h>
#include <time.h>
#include <driver/i2c.h>

namespace eego {
struct RtcTime {
    uint16_t year;
    uint8_t month;    // 1-12
    uint8_t day;      // 1-31
    uint8_t hour;     // 0-23
    uint8_t minute;   // 0-59
    uint8_t second;   // 0-59
    uint8_t weekday;
    bool valid;
};

class Pcf8563Rtc {
public:
    Pcf8563Rtc(i2c_port_t port = I2C_NUM_0);

    esp_err_t begin();
    RtcTime readTime();
    esp_err_t writeTime(const RtcTime& t);
    time_t toEpoch(const RtcTime& t);

private:
    uint8_t bcdToBin(uint8_t bcd);
    uint8_t binToBcd(uint8_t bin);
    esp_err_t readReg(uint8_t reg, uint8_t* data, size_t len);
    esp_err_t writeReg(uint8_t reg, const uint8_t* data, size_t len);

    i2c_port_t port_ = I2C_NUM_0;
};

}  // namespace eego
