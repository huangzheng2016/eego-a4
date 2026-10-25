// SPDX-License-Identifier: GPL-3.0-or-later
// 移植自 eego-a4-weread（GPL-3.0）同名文件；原始硬件事实来自 eego-a4-template-firmware（MIT）。
// EEGO A4 前光驱动 — LM3630A (I2C 0x36)，GPIO12 使能
// 纯 ESP-IDF，使用 legacy driver/i2c.h API
// 寄存器序列参考 TI SNVS974B
#pragma once
#include <stdint.h>
#include "eego_pins.h"
#include <esp_err.h>
#include <driver/gpio.h>
#include <driver/i2c.h>

namespace eego {

// 用户覆盖（持久化在 device_settings 的 frontlight_hardware）。
enum class FrontlightOverride : uint8_t { Auto = 0, Present = 1, Absent = 2 };

class FrontlightManager {
public:
    static FrontlightManager& instance();

    // 芯片探测（缓存；最多 3 次尝试，ACK + 亮度寄存器写/回读校验并恢复原值）。
    // 仅反映 LM3630A 硬件，驱动 PowerManager 的充电状态引脚极性。
    bool lm3630aChipPresent();
    // 前光是否可用 = 覆盖设置（Present/Absent）或 Auto 时的芯片探测结果。
    // UI/能力门控应使用此函数；不可用时 setBrightness/setWarm/on 为空操作。
    bool frontlightAvailable();

    void setOverride(FrontlightOverride mode);  // 变为不可用时立即关闭前光
    FrontlightOverride overrideMode() const { return _override; }

    void setBrightness(uint8_t percent);  // 0-100，0=关闭前光
    void setWarm(uint8_t warmPercent);    // 0=全冷，100=全暖
    void off();                           // 关闭（等效 setBrightness(0)）
    void on();                            // 恢复上一次亮度

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