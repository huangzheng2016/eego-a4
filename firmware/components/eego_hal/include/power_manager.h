// SPDX-License-Identifier: GPL-3.0-or-later
// 移植自 eego-a4-weread（GPL-3.0）同名文件；原始硬件事实来自 eego-a4-template-firmware（MIT）。
// EEGO A4 电源管理 — GPIO4 锁存 + 电池 ADC + 充电状态
// 纯 ESP-IDF，无 Arduino 依赖
#pragma once
#include "eego_pins.h"
#include <esp_err.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <driver/gpio.h>

namespace eego {

struct BatteryInfo {
    uint16_t millivolts;     // 实际电池电压
    uint8_t percent;         // 0-100
    bool charging;           // 是否充电中
};

// GPIO11 充电状态极性。Auto = 按 LM3630A 在位推断（在位 → 高有效，否则低有效）。
enum class ChargePolarity : uint8_t { Auto = 0, ActiveHigh, ActiveLow };

class PowerManager {
public:
    static PowerManager& instance();

    void holdPower();        // GPIO4 锁存（系统启动后首项操作！）
    BatteryInfo readBattery();
    bool isCharging();
    // 覆盖充电脚极性（device_settings charge_signal）：立即重配 GPIO11 上/下拉。
    void setChargePolarity(ChargePolarity mode);
    bool chargeActiveHigh() { ensureInitialized(); return chargeActiveHigh_; }
    // USB 主机已连接（USB-Serial-JTAG 收到 SOF：插着电脑/带数据的 USB 口）。
    // 注意：纯充电头不发 SOF —— 外部供电判定见 main/battery_policy MainsDetector。
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
    static uint8_t percentFromMillivolts(uint16_t mv);
};

}  // namespace eego
