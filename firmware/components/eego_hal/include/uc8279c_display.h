// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eego_pins.h"

#include <driver/spi_master.h>
#include <esp_err.h>
#include <stdint.h>

namespace eego {
enum class RefreshMode { Fast, Full };

class Uc8279cDisplay {
public:
    Uc8279cDisplay();
    ~Uc8279cDisplay();

    esp_err_t begin(bool deferController = false);
    esp_err_t beginOffscreen();
    bool offscreen() const { return offscreen_; }
    void end();

    void clear();
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color);
    void drawPixel(int16_t x, int16_t y, uint8_t color);
    uint8_t getPixel(int16_t x, int16_t y) const;

    void drawPixelGray(int16_t x, int16_t y, uint8_t gray);
    void clearGray();
    bool hasGrayContent() const;
    uint8_t grayLevel(int16_t x, int16_t y) const;
    static uint8_t grayToBw(uint8_t gray) { return gray >= 8 ? 1 : 0; }

    void display(RefreshMode mode = RefreshMode::Fast, bool turnOff = false);
    void waitRefreshComplete();
    void displayGray(bool turnOff = false);

    void setPreviousFrame(const uint8_t* nativeFrame) { prevFrame_ = nativeFrame; }

    void requestFullRefresh() { fullRequested_ = true; }
    void setFastRefreshLimit(uint8_t n) { fastRefreshLimit_ = n; }
    uint8_t fastRefreshLimit() const { return fastRefreshLimit_; }
    static bool refreshActive();

    void setAutoPowerOff(bool on) { autoPowerOff_ = on; }
    bool autoPowerOff() const { return autoPowerOff_; }

    void deepSleep();

    static constexpr uint16_t WIDTH = LOGICAL_WIDTH;   // 552
    static constexpr uint16_t HEIGHT = LOGICAL_HEIGHT; // 768

    uint8_t* framebuffer() { return fb_; }
    const uint8_t* framebuffer() const { return fb_; }
    const uint8_t* grayMsbPlane() const { return planesActive_ ? fbGrayMsb_ : nullptr; }
    const uint8_t* grayLsbPlane() const { return planesActive_ ? fbGrayLsb_ : nullptr; }

private:
    esp_err_t spiInit();
    void gpioInit();
    void hardwareReset();
    void initController(bool grayMode = false);
    void powerUpController();
    void ensureController();
    void ensurePowerOn();
    void fillControllerRam(uint8_t cmd, uint8_t fill);
    void loadFullLut();
    void loadFastLut();
    void loadGrayLut();
    void writeFrame(uint8_t cmd, const uint8_t* fb);
    bool refresh(bool turnOff);
    void waitBusy(const char* tag);
    void waitIfBusy(const char* tag);
    void displayBw(bool full, bool turnOff);
    void displayGrayFrame(bool turnOff);
    void setGrayPlanes(uint32_t byteIdx, uint8_t bit, uint8_t level);  // level 0-3
    void sendCmd(uint8_t cmd);
    void sendData(uint8_t data);
    void sendData(const uint8_t* data, size_t len);
    void cmdData(uint8_t cmd, const uint8_t* data, size_t len);
    void spiTransmit(const uint8_t* data, size_t len);
    void ensureGrayBuffers();

    spi_device_handle_t spi_ = nullptr;
    uint8_t* fb_ = nullptr;
    uint8_t* fbGrayMsb_ = nullptr;
    uint8_t* fbGrayLsb_ = nullptr;
    bool planesActive_ = false;
    bool offscreen_ = false;
    bool screenOn_ = false;
    bool grayControllerMode_ = false;
    bool fullRequested_ = false;
    bool controllerPending_ = false;
    const uint8_t* prevFrame_ = nullptr;
    bool autoPowerOff_ = true;
    uint8_t fastRefreshesSinceFull_ = 0;
    uint8_t fastRefreshLimit_ = DEFAULT_FAST_REFRESH_LIMIT;
    static constexpr uint8_t DEFAULT_FAST_REFRESH_LIMIT = 8;
};

}  // namespace eego
