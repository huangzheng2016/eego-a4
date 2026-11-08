// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontlight.h"

#include <esp_log.h>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* FL_TAG = "eego_fl";

static void flDelayMs(uint32_t ms) {
    TickType_t ticks = pdMS_TO_TICKS(ms);
    if (ticks == 0) ticks = 1;
    vTaskDelay(ticks);
}

static esp_err_t flProbeAck(uint8_t addr) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, static_cast<uint8_t>((addr << 1) | I2C_MASTER_WRITE), true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return err;
}

static bool flBusInstalled() {
    return flProbeAck(eego::GSLX680_ADDRESS) == ESP_OK ||
           flProbeAck(eego::PCF8563_ADDRESS) == ESP_OK;
}

static bool s_flBusReady = false;
static esp_err_t flEnsureI2cBus() {
    if (s_flBusReady) return ESP_OK;
    if (flBusInstalled()) {
        s_flBusReady = true;
        return ESP_OK;
    }
    i2c_config_t conf;
    memset(&conf, 0, sizeof(conf));
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = static_cast<gpio_num_t>(eego::I2C_SDA);
    conf.scl_io_num = static_cast<gpio_num_t>(eego::I2C_SCL);
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = eego::I2C_HZ;
    esp_err_t err = i2c_param_config(I2C_NUM_0, &conf);
    if (err != ESP_OK) return err;
    err = i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    s_flBusReady = true;
    return ESP_OK;
}

static constexpr int kProbeAttempts = 3;
static constexpr uint32_t kProbeRetryDelayMs = 8;
static constexpr uint8_t kProbeReg = 0x03;
static constexpr uint8_t kProbePattern = 0x5a;

namespace eego {
FrontlightManager& FrontlightManager::instance() {
    static FrontlightManager inst;
    return inst;
}

bool FrontlightManager::probeOnce() {
    gpio_set_level(static_cast<gpio_num_t>(FRONTLIGHT_ENABLE), 0);
    flDelayMs(1);
    gpio_set_level(static_cast<gpio_num_t>(FRONTLIGHT_ENABLE), 1);
    flDelayMs(2);
    if (flProbeAck(eego::LM3630A_ADDRESS) != ESP_OK) return false;

    uint8_t original = 0;
    if (lm3630aRead(kProbeReg, &original) != ESP_OK) return false;
    const uint8_t pattern = original == kProbePattern
                                ? static_cast<uint8_t>(~kProbePattern)
                                : kProbePattern;
    uint8_t readback = 0;
    const bool ok = lm3630aWrite(kProbeReg, pattern) == ESP_OK &&
                    lm3630aRead(kProbeReg, &readback) == ESP_OK &&
                    readback == pattern;
    lm3630aWrite(kProbeReg, original);
    return ok;
}

bool FrontlightManager::lm3630aChipPresent() {
    if (_presentCached_) return _present_;

    gpio_config_t io_conf;
    memset(&io_conf, 0, sizeof(io_conf));
    io_conf.pin_bit_mask = (1ULL << FRONTLIGHT_ENABLE);
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);

    bool present = false;
    if (flEnsureI2cBus() == ESP_OK) {
        for (int attempt = 0; attempt < kProbeAttempts && !present; ++attempt) {
            if (attempt > 0) flDelayMs(kProbeRetryDelayMs);
            present = probeOnce();
            if (!present) {
                ESP_LOGD(FL_TAG, "LM3630A probe attempt %d failed", attempt + 1);
            }
        }
    }
    gpio_set_level(static_cast<gpio_num_t>(FRONTLIGHT_ENABLE), 0);
    _i2cConfigured = false;

    _presentCached_ = true;
    _present_ = present;
    ESP_LOGI(FL_TAG, "LM3630A %s", present ? "detected" : "not detected");
    return present;
}

bool FrontlightManager::frontlightAvailable() {
    switch (_override) {
        case FrontlightOverride::Present: return true;
        case FrontlightOverride::Absent: return false;
        case FrontlightOverride::Auto: break;
    }
    return lm3630aChipPresent();
}

void FrontlightManager::setOverride(FrontlightOverride mode) {
    _override = mode;
    if (!frontlightAvailable()) off();
}

esp_err_t FrontlightManager::lm3630aWrite(uint8_t reg, uint8_t value) {
    uint8_t buf[2] = {reg, value};
    return i2c_master_write_to_device(
        I2C_NUM_0, eego::LM3630A_ADDRESS, buf, sizeof(buf), pdMS_TO_TICKS(100));
}

esp_err_t FrontlightManager::lm3630aRead(uint8_t reg, uint8_t* value) {
    return i2c_master_write_read_device(
        I2C_NUM_0, eego::LM3630A_ADDRESS, &reg, 1, value, 1, pdMS_TO_TICKS(100));
}

esp_err_t FrontlightManager::lm3630aUpdate(uint8_t reg, uint8_t mask, uint8_t value) {
    uint8_t cur = 0;
    esp_err_t err = lm3630aRead(reg, &cur);
    if (err != ESP_OK) return err;
    uint8_t next = static_cast<uint8_t>((cur & ~mask) | (value & mask));
    return lm3630aWrite(reg, next);
}

bool FrontlightManager::configureLm3630a() {
    // enable HIGH, delay 2ms
    gpio_set_level(static_cast<gpio_num_t>(FRONTLIGHT_ENABLE), 1);
    flDelayMs(2);

    esp_err_t err = flProbeAck(eego::LM3630A_ADDRESS);
    if (err != ESP_OK) {
        gpio_set_level(static_cast<gpio_num_t>(FRONTLIGHT_ENABLE), 0);
        return false;
    }

    lm3630aWrite(0x50, 0x03);
    lm3630aUpdate(0x01, 0x07, 0x00);
    lm3630aWrite(0x02, 0x38);
    lm3630aUpdate(0x05, 0x1f, 0x10);
    lm3630aUpdate(0x06, 0x1f, 0x10);
    lm3630aUpdate(0x00, 0x14, 0x00);
    lm3630aUpdate(0x00, 0x0b, 0x00);
    flDelayMs(2);
    lm3630aWrite(0x03, 0x00);
    lm3630aWrite(0x04, 0x00);
    return true;
}

void FrontlightManager::applyLm3630a() {
    if (_brightness == 0) {
        off();
        return;
    }
    if (!_i2cConfigured) {
        if (!configureLm3630a()) return;
        _i2cConfigured = true;
    }

    uint8_t level = static_cast<uint8_t>(_brightness * 255 / 100);
    uint8_t warm = static_cast<uint8_t>(level * _warmPercent / 100);
    uint8_t cool = static_cast<uint8_t>(level * (100 - _warmPercent) / 100);
    if (warm != 0 && warm < 4) warm = 4;
    if (cool != 0 && cool < 4) cool = 4;

    lm3630aUpdate(0x00, 0x80, 0x00);
    flDelayMs(2);
    lm3630aWrite(0x03, warm);
    lm3630aWrite(0x04, cool);
    lm3630aUpdate(0x00, 0x04, warm >= 4 ? 0x04 : 0x00);
    lm3630aUpdate(0x00, 0x02, cool >= 4 ? 0x02 : 0x00);

    ESP_LOGD(FL_TAG, "frontlight brt=%u warm=%u (level=%u w=%u c=%u)",
             _brightness, _warmPercent, level, warm, cool);
}

void FrontlightManager::setBrightness(uint8_t percent) {
    if (percent > 100) percent = 100;
    _brightness = percent;
    if (_brightness == 0) {
        off();
        return;
    }
    if (!frontlightAvailable()) return;
    if (flEnsureI2cBus() != ESP_OK) return;
    applyLm3630a();
}

void FrontlightManager::setWarm(uint8_t warmPercent) {
    if (warmPercent > 100) warmPercent = 100;
    _warmPercent = warmPercent;
    if (_brightness > 0 && frontlightAvailable()) applyLm3630a();
}

void FrontlightManager::off() {
    if (_i2cConfigured) {
        lm3630aWrite(0x03, 0x00);
        lm3630aWrite(0x04, 0x00);
    }
    gpio_set_level(static_cast<gpio_num_t>(FRONTLIGHT_ENABLE), 0);
    _i2cConfigured = false;
}

void FrontlightManager::on() {
    if (_brightness == 0) _brightness = 50;
    if (!frontlightAvailable()) return;
    if (flEnsureI2cBus() != ESP_OK) return;
    applyLm3630a();
}

}  // namespace eego