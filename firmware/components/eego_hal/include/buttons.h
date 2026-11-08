// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <esp_err.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <atomic>
#include "input_events.h"

namespace eego {
class ButtonManager {
public:
    static ButtonManager& instance();

    esp_err_t begin();
    void end();

    bool poll(ButtonEvent* event, uint32_t timeoutMs = 0);

    void setDoublePressEnabled(ButtonId id, bool enabled);
    bool doublePressEnabled(ButtonId id) const;

    void setWakeSemaphore(SemaphoreHandle_t wake) { wake_.store(wake); }

private:
    ButtonManager() = default;
    static void pollTask(void* arg);
    QueueHandle_t queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<uint8_t> doubleMask_{0};
    std::atomic<SemaphoreHandle_t> wake_{nullptr};
};

}  // namespace eego
