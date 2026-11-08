// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include "eego_pins.h"
#include <esp_err.h>
#include <driver/gpio.h>
#include <driver/i2c.h>

namespace eego {
enum class FrontlightOverride : uint8_t { Auto = 0, Present = 1, Absent = 2 };

class FrontlightManager {
public:
    static FrontlightManager& instance();

    bool lm3630aChipPresent();
    bool frontlightAvailable();

    void setOverride(FrontlightOverride mode);
    FrontlightOverride overrideMode() const { return _override; }

    void setBrightness(uint8_t percent);
    void setWarm(uint8_t warmPercent);
    void off();
    void on();

private:
    FrontlightManager() = default;
    bool probeOnce();
    FrontlightOverride _override = FrontlightOverride::Auto;

    uint8_t _brightness = 0;
    uint8_t _warmPercent = 0;
    bool _i2cConfigured = false;
    bool _presentCached_ = false;
    bool _present_ = false;

    esp_err_t lm3630aWrite(uint8_t reg, uint8_t value);
    esp_err_t lm3630aRead(uint8_t reg, uint8_t* value);
    esp_err_t lm3630aUpdate(uint8_t reg, uint8_t mask, uint8_t value);
    bool configureLm3630a();
    void applyLm3630a();
};

}  // namespace eego