// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "eego_pins.h"
#include <esp_err.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <driver/gpio.h>

namespace eego {
struct BatteryInfo {
    uint16_t millivolts;
    uint8_t percent;         // 0-100
    bool charging;
};

enum class ChargePolarity : uint8_t { Auto = 0, ActiveHigh, ActiveLow };

class PowerManager {
public:
    static PowerManager& instance();

    void holdPower();
    BatteryInfo readBattery();
    bool isCharging();
    void setChargePolarity(ChargePolarity mode);
    bool chargeActiveHigh() { ensureInitialized(); return chargeActiveHigh_; }
    bool usbHostConnected();
    bool hasFrontlight() const { return lm3630aPresent_; }

private:
    PowerManager() = default;
    bool initialized_ = false;
    adc_oneshot_unit_handle_t adcUnit_ = nullptr;
    adc_cali_handle_t adcCali_ = nullptr;
    bool lm3630aPresent_ = false;
    bool chargeActiveHigh_ = false;
    ChargePolarity chargePolarity_ = ChargePolarity::Auto;

    void initAdc();
    void detectLm3630a();
    void configureChargePin();
    void ensureInitialized();

public:
    static uint8_t percentFromMillivolts(uint16_t mv);
};

}  // namespace eego
