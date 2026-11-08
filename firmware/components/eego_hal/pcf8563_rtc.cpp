// SPDX-License-Identifier: GPL-3.0-or-later
//   0x00: Control/Status 1
//   0x01: Control/Status 2
#include "pcf8563_rtc.h"

#include <esp_log.h>
#include <cstring>

static const char* RTC_TAG = "eego_rtc";

static constexpr uint8_t PCF8563_REG_CONTROL_STATUS1 = 0x00;
static constexpr uint8_t PCF8563_REG_CONTROL_STATUS2 = 0x01;
static constexpr uint8_t PCF8563_REG_VL_SECONDS      = 0x02;
static constexpr uint8_t PCF8563_REG_CLKOUT          = 0x0D;
static constexpr uint8_t PCF8563_CLKOUT_DISABLED     = 0x00;
static constexpr uint8_t PCF8563_VL_FLAG             = 0x80;

static esp_err_t rtcProbeAck(i2c_port_t port, uint8_t addr) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, static_cast<uint8_t>((addr << 1) | I2C_MASTER_WRITE), true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return err;
}

static bool rtcBusInstalled(i2c_port_t port) {
    return rtcProbeAck(port, eego::GSLX680_ADDRESS) == ESP_OK ||
           rtcProbeAck(port, eego::PCF8563_ADDRESS) == ESP_OK;
}

static esp_err_t rtcEnsureI2cBus(i2c_port_t port) {
    if (rtcBusInstalled(port)) return ESP_OK;
    i2c_config_t conf;
    memset(&conf, 0, sizeof(conf));
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = static_cast<gpio_num_t>(eego::I2C_SDA);
    conf.scl_io_num = static_cast<gpio_num_t>(eego::I2C_SCL);
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = eego::I2C_HZ;
    esp_err_t err = i2c_param_config(port, &conf);
    if (err != ESP_OK) return err;
    err = i2c_driver_install(port, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE && err != ESP_FAIL) return err;
    return ESP_OK;
}

namespace eego {
Pcf8563Rtc::Pcf8563Rtc(i2c_port_t port) : port_(port) {}

uint8_t Pcf8563Rtc::bcdToBin(uint8_t bcd) {
    return static_cast<uint8_t>((bcd >> 4) * 10U + (bcd & 0x0FU));
}

uint8_t Pcf8563Rtc::binToBcd(uint8_t bin) {
    return static_cast<uint8_t>(((bin / 10U) << 4) | (bin % 10U));
}

esp_err_t Pcf8563Rtc::readReg(uint8_t reg, uint8_t* data, size_t len) {
    return i2c_master_write_read_device(
        port_, PCF8563_ADDRESS, &reg, 1, data, len, pdMS_TO_TICKS(200));
}

esp_err_t Pcf8563Rtc::writeReg(uint8_t reg, const uint8_t* data, size_t len) {
    uint8_t buf[16];
    if (len + 1 > sizeof(buf)) return ESP_ERR_INVALID_SIZE;
    buf[0] = reg;
    memcpy(&buf[1], data, len);
    return i2c_master_write_to_device(
        port_, PCF8563_ADDRESS, buf, len + 1, pdMS_TO_TICKS(200));
}

esp_err_t Pcf8563Rtc::begin() {
    esp_err_t err = rtcEnsureI2cBus(port_);
    if (err != ESP_OK) {
        ESP_LOGE(RTC_TAG, "I2C bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t status1 = 0;
    err = readReg(PCF8563_REG_CONTROL_STATUS1, &status1, 1);
    if (err != ESP_OK) {
        ESP_LOGE(RTC_TAG, "PCF8563 not responding at 0x%02X: %s",
                 PCF8563_ADDRESS, esp_err_to_name(err));
        return err;
    }

    uint8_t clkoutOff = PCF8563_CLKOUT_DISABLED;
    writeReg(PCF8563_REG_CLKOUT, &clkoutOff, 1);

    ESP_LOGI(RTC_TAG, "PCF8563 initialized at 0x%02X", PCF8563_ADDRESS);
    return ESP_OK;
}

RtcTime Pcf8563Rtc::readTime() {
    RtcTime t;
    memset(&t, 0, sizeof(t));
    t.valid = false;

    uint8_t raw[7];
    memset(raw, 0, sizeof(raw));
    esp_err_t err = readReg(PCF8563_REG_VL_SECONDS, raw, sizeof(raw));
    if (err != ESP_OK) {
        ESP_LOGW(RTC_TAG, "RTC read failed: %s", esp_err_to_name(err));
        return t;
    }

    bool vl = (raw[0] & PCF8563_VL_FLAG) != 0;

    t.second  = bcdToBin(raw[0] & 0x7FU);
    t.minute  = bcdToBin(raw[1] & 0x7FU);
    t.hour    = bcdToBin(raw[2] & 0x3FU);
    t.day     = bcdToBin(raw[3] & 0x3FU);
    t.weekday = bcdToBin(raw[4] & 0x07U);
    t.month   = bcdToBin(raw[5] & 0x1FU);

    uint8_t yy = bcdToBin(raw[6]);
    t.year = (raw[5] & 0x80U) ? static_cast<uint16_t>(1900 + yy)
                              : static_cast<uint16_t>(2000 + yy);

    const bool fieldsValid =
        t.second <= 59 && t.minute <= 59 && t.hour <= 23 &&
        t.day >= 1 && t.day <= 31 && t.weekday <= 6 &&
        t.month >= 1 && t.month <= 12 && t.year >= 1900 && t.year <= 2099;
    t.valid = !vl && fieldsValid;
    if (!vl && !fieldsValid) {
        ESP_LOGW(RTC_TAG, "RTC time fields are invalid");
    }

    if (vl) {
        uint8_t secClear = binToBcd(t.second) & 0x7FU;
        writeReg(PCF8563_REG_VL_SECONDS, &secClear, 1);
        ESP_LOGW(RTC_TAG, "RTC VL flag was set (oscillator stopped), cleared");
    }

    return t;
}

esp_err_t Pcf8563Rtc::writeTime(const RtcTime& t) {
    uint8_t raw[7];
    raw[0] = binToBcd(t.second) & 0x7FU;
    raw[1] = binToBcd(t.minute) & 0x7FU;
    raw[2] = binToBcd(t.hour) & 0x3FU;
    raw[3] = binToBcd(t.day) & 0x3FU;
    raw[4] = binToBcd(t.weekday) & 0x07U;
    uint8_t centuryBit = (t.year < 2000) ? 0x80U : 0x00U;
    raw[5] = static_cast<uint8_t>(binToBcd(t.month) | centuryBit);
    raw[6] = binToBcd(static_cast<uint8_t>(t.year % 100));

    esp_err_t err = writeReg(PCF8563_REG_VL_SECONDS, raw, sizeof(raw));
    if (err != ESP_OK) {
        ESP_LOGE(RTC_TAG, "RTC write failed: %s", esp_err_to_name(err));
    }
    return err;
}

time_t Pcf8563Rtc::toEpoch(const RtcTime& t) {
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = t.year - 1900;
    tm.tm_mon = t.month - 1;
    tm.tm_mday = t.day;
    tm.tm_hour = t.hour;
    tm.tm_min = t.minute;
    tm.tm_sec = t.second;
    tm.tm_wday = t.weekday;
    return mktime(&tm);
}

}  // namespace eego
