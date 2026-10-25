// SPDX-License-Identifier: GPL-3.0-or-later
// 移植自 eego-a4-weread（GPL-3.0）同名文件；原始硬件事实来自 eego-a4-template-firmware（MIT）。
// EEGO A4 按键管理 — 三键异步输入队列
// 纯 ESP-IDF，FreeRTOS 后台轮询任务
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

    esp_err_t begin();  // 初始化 GPIO + 启动后台轮询任务
    void end();

    bool poll(ButtonEvent* event, uint32_t timeoutMs = 0);  // 从队列取事件

    // Up/Down 双击检测（默认关闭 → ShortPress 在释放时立即发出）。仅当存在
    // up.double / down.double 绑定时开启；Power 永不等待双击。任意任务可调用。
    void setDoublePressEnabled(ButtonId id, bool enabled);
    bool doublePressEnabled(ButtonId id) const;

    // 入队事件后 give 该二值信号量，唤醒消费者（app_task）。可为 nullptr。
    void setWakeSemaphore(SemaphoreHandle_t wake) { wake_.store(wake); }

private:
    ButtonManager() = default;
    static void pollTask(void* arg);
    QueueHandle_t queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<uint8_t> doubleMask_{0};  // bit i = kButtons[i] 开启双击
    std::atomic<SemaphoreHandle_t> wake_{nullptr};
};

}  // namespace eego
