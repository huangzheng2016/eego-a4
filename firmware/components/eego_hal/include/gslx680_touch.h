// SPDX-License-Identifier: GPL-3.0-or-later
//
#pragma once
#include "eego_pins.h"
#include <stdint.h>
#include <esp_err.h>
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "input_gesture.h"
#include "touch_calibration.h"

#include <atomic>
#include <mutex>
#include <vector>

namespace eego {
class Gslx680Touch {
public:
    Gslx680Touch();
    ~Gslx680Touch();

    esp_err_t begin(i2c_port_t port = I2C_NUM_0);
    void end();
    // Stops polling and holds the controller's MCU in reset for deep sleep; the next
    // begin() reloads its firmware.
    void sleep();

    esp_err_t startPolling(SemaphoreHandle_t wake = nullptr);
    bool stopPolling();
    bool pollingActive() const { return queue_ != nullptr && running_.load(); }

    TouchEvent poll();

    bool isPressed() const { return pressed_.load(); }
    bool hasPending() const;
    uint32_t droppedEvents() const { return dropped_.load(); }

    touchcal::Affine calibration() const;
    bool calibrationCustom() const;
    bool applyCalibration(const touchcal::Affine& affine);
    esp_err_t saveCalibration(const touchcal::Affine& affine);
    esp_err_t clearCalibration();

private:
    static void pollTask(void* arg);
    std::vector<TouchEvent> sampleFrame();
    esp_err_t writeReg(uint8_t reg, const uint8_t* data, size_t len);
    esp_err_t writeReg32(uint8_t reg, uint32_t value);
    esp_err_t readReg(uint8_t reg, uint8_t* data, size_t len);
    esp_err_t uploadFirmware();
    void clearRegisters();
    void resetChip();
    void startChip();
    bool checkChip();

    i2c_port_t i2cPort_ = I2C_NUM_0;
    bool initialized_ = false;
    bool enabled_ = false;
    TouchGestureFsm gestures_;
    std::vector<TouchEvent> pending_;
    QueueHandle_t queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> taskExited_{true};
    std::atomic<bool> pressed_{false};
    std::atomic<uint32_t> dropped_{0};
    SemaphoreHandle_t wake_ = nullptr;

    mutable std::mutex calMutex_;
    touchcal::Affine cal_ = touchcal::defaultAffine();
    uint32_t rawSumX_ = 0;
    uint32_t rawSumY_ = 0;
    uint16_t rawCount_ = 0;
};

}  // namespace eego
