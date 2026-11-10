// SPDX-License-Identifier: GPL-3.0-or-later
#include "power_manager.h"

#include <algorithm>
#include "frontlight.h"

#include <esp_log.h>
#include <esp_sleep.h>
#include <driver/i2c.h>
#include <esp_adc/adc_cali_scheme.h>
#include <cstring>
#include "sdkconfig.h"
#if __has_include("driver/usb_serial_jtag.h")
#include "driver/usb_serial_jtag.h"
#define EEGO_HAS_USB_SERIAL_JTAG 1
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* PM_TAG = "eego_pm";

namespace eego {
PowerManager& PowerManager::instance() {
    static PowerManager inst;
    return inst;
}

void PowerManager::holdPower() {
    gpio_config_t io_conf;
    memset(&io_conf, 0, sizeof(io_conf));
    io_conf.pin_bit_mask = (1ULL << POWER_LATCH);
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);

    gpio_hold_dis(static_cast<gpio_num_t>(POWER_LATCH));
    gpio_set_level(static_cast<gpio_num_t>(POWER_LATCH), 1);
    gpio_hold_en(static_cast<gpio_num_t>(POWER_LATCH));
    gpio_deep_sleep_hold_en();

    ESP_LOGI(PM_TAG, "Power latch engaged on GPIO%d", POWER_LATCH);
}

void PowerManager::initAdc() {
    if (adcUnit_) return;

    // GPIO10 = ADC1_CH9 (ESP32-S3)
    adc_oneshot_unit_init_cfg_t initCfg;
    memset(&initCfg, 0, sizeof(initCfg));
    initCfg.unit_id = ADC_UNIT_1;
    esp_err_t err = adc_oneshot_new_unit(&initCfg, &adcUnit_);
    if (err != ESP_OK) {
        ESP_LOGE(PM_TAG, "ADC unit init failed: %s", esp_err_to_name(err));
        return;
    }

    adc_oneshot_chan_cfg_t chanCfg;
    memset(&chanCfg, 0, sizeof(chanCfg));
    chanCfg.bitwidth = ADC_BITWIDTH_12;
    chanCfg.atten = ADC_ATTEN_DB_12;
    err = adc_oneshot_config_channel(adcUnit_, ADC_CHANNEL_9, &chanCfg);
    if (err != ESP_OK) {
        ESP_LOGE(PM_TAG, "ADC channel config failed: %s", esp_err_to_name(err));
    }

    adc_cali_curve_fitting_config_t caliCfg;
    memset(&caliCfg, 0, sizeof(caliCfg));
    caliCfg.unit_id = ADC_UNIT_1;
    caliCfg.chan = ADC_CHANNEL_9;
    caliCfg.atten = ADC_ATTEN_DB_12;
    caliCfg.bitwidth = ADC_BITWIDTH_12;
    err = adc_cali_create_scheme_curve_fitting(&caliCfg, &adcCali_);
    if (err != ESP_OK) {
        ESP_LOGW(PM_TAG, "ADC calibration failed (using raw): %s", esp_err_to_name(err));
        adcCali_ = nullptr;
    }
}

void PowerManager::detectLm3630a() {
    lm3630aPresent_ = FrontlightManager::instance().lm3630aChipPresent();
    configureChargePin();
}

void PowerManager::configureChargePin() {
    switch (chargePolarity_) {
        case ChargePolarity::ActiveHigh: chargeActiveHigh_ = true; break;
        case ChargePolarity::ActiveLow: chargeActiveHigh_ = false; break;
        // Measured on real hardware: the old mapping (LM3630A present → active-high)
        // read "charging" on battery and never let the panel deep-sleep.
        case ChargePolarity::Auto: chargeActiveHigh_ = !lm3630aPresent_; break;
    }

    gpio_config_t io_conf;
    memset(&io_conf, 0, sizeof(io_conf));
    io_conf.pin_bit_mask = (1ULL << CHARGE_STATUS);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    if (chargeActiveHigh_) {
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    } else {
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    }
    gpio_config(&io_conf);

    ESP_LOGI(PM_TAG, "LM3630A %s, charge status active-%s (%s)",
             lm3630aPresent_ ? "present" : "absent",
             chargeActiveHigh_ ? "high" : "low",
             chargePolarity_ == ChargePolarity::Auto ? "auto" : "forced");
}

void PowerManager::setChargePolarity(ChargePolarity mode) {
    ensureInitialized();
    if (mode == chargePolarity_) return;
    chargePolarity_ = mode;
    configureChargePin();
}

void PowerManager::ensureInitialized() {
    if (initialized_) return;
    initAdc();
    detectLm3630a();
    initialized_ = true;
}

uint8_t PowerManager::percentFromMillivolts(uint16_t mv) {
    // 3.00V→0%, 3.70V→20%, 3.80V→50%, 4.00V→80%, 4.10V→100%
    if (mv >= 4100) return 100;
    if (mv <= 3000) return 0;

    struct Point { uint16_t mv; uint8_t pct; };
    static constexpr Point points[] = {
        {3000, 0}, {3700, 20}, {3800, 50}, {4000, 80}, {4100, 100}
    };
    constexpr size_t N = sizeof(points) / sizeof(points[0]);

    for (size_t i = 1; i < N; i++) {
        if (mv <= points[i].mv) {
            const Point& p0 = points[i - 1];
            const Point& p1 = points[i];
            float frac = static_cast<float>(mv - p0.mv) /
                         static_cast<float>(p1.mv - p0.mv);
            float pct = p0.pct + frac * static_cast<float>(p1.pct - p0.pct);
            if (pct < 0.0f) pct = 0.0f;
            if (pct > 100.0f) pct = 100.0f;
            return static_cast<uint8_t>(pct + 0.5f);
        }
    }
    return 100;
}

int PowerManager::sampleMillivolts() {
    // 16 samples, the 4 highest and 4 lowest dropped: one noisy sample (radio burst,
    // panel charge pump) must not swing the reading toward a false low battery.
    constexpr int kSamples = 16;
    constexpr int kTrim = 4;
    int raws[kSamples];
    for (int i = 0; i < kSamples; i++) {
        if (adc_oneshot_read(adcUnit_, ADC_CHANNEL_9, &raws[i]) != ESP_OK) return -1;
    }
    std::sort(raws, raws + kSamples);
    int rawSum = 0;
    for (int i = kTrim; i < kSamples - kTrim; i++) rawSum += raws[i];
    const int avgRaw = rawSum / (kSamples - 2 * kTrim);

    int adcMv = 0;
    if (adcCali_) {
        adc_cali_raw_to_voltage(adcCali_, avgRaw, &adcMv);
    } else {
        adcMv = static_cast<int>(static_cast<float>(avgRaw) * 3300.0f / 4095.0f);
    }
    return static_cast<int>(static_cast<float>(adcMv) * BATTERY_DIVIDER);
}

BatteryInfo PowerManager::readBattery() {
    ensureInitialized();

    BatteryInfo info;
    info.millivolts = 0;
    info.percent = 0;
    info.charging = false;

    if (!adcUnit_) return info;

    // The battery sense divider reads near zero while the panel rail (EPD_POWER_ENABLE)
    // is off: interactive wakes (rail on) read 94% while timer wakes (rail off until a
    // redraw) read 0% and entered critical sleep. Read with the rail on; switch it on
    // briefly when the panel is unpowered and restore it afterwards.
    const gpio_num_t rail = static_cast<gpio_num_t>(EPD_POWER_ENABLE);
    const bool railOff = gpio_get_level(rail) == 0;
    int offMv = -1;
    if (railOff) {
        offMv = sampleMillivolts();
        gpio_hold_dis(rail);
        gpio_set_level(rail, 1);
        vTaskDelay(pdMS_TO_TICKS(kRailSettleMs));
    }
    const int mv = sampleMillivolts();
    if (railOff) {
        gpio_set_level(rail, 0);
        ESP_LOGI(PM_TAG, "battery: %dmV with panel rail on (%dmV with it off)", mv, offMv);
    }
    if (mv < 0) return info;

    info.millivolts = static_cast<uint16_t>(mv);
    info.percent = percentFromMillivolts(info.millivolts);
    info.charging = isCharging();

    ESP_LOGD(PM_TAG, "Battery: %umV → %u%%", info.millivolts, info.percent);
    return info;
}

bool PowerManager::isCharging() {
    ensureInitialized();

    int level = gpio_get_level(static_cast<gpio_num_t>(CHARGE_STATUS));
    return chargeActiveHigh_ ? (level == 1) : (level == 0);
}

bool PowerManager::usbHostConnected() {
#if defined(EEGO_HAS_USB_SERIAL_JTAG) && CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED
    return usb_serial_jtag_is_connected();
#else
    return false;
#endif
}

}  // namespace eego
