// SPDX-License-Identifier: GPL-3.0-or-later
// 移植自 eego-a4-weread（GPL-3.0）同名文件；原始硬件事实来自 eego-a4-template-firmware（MIT）。
// GSLX680 电容触控驱动（EEGO A4）— 纯 ESP-IDF
//
// 轮询式触控驱动：startPolling() 后由独立任务每 tick(10ms) 读取 24 字节触控帧，
// 经手势 FSM 后写入 FreeRTOS 队列；app_task 用 poll() 非阻塞取事件。
// 渲染阻塞 app_task 期间采样不中断，滑动不会被压缩成同坐标的 Tap。
// 未启动任务时 poll() 退回旧的同步读帧模式。
// 坐标经 eego_pins.h 的 rawTouchToLogicalX/Y 映射为 portrait 逻辑坐标。
// 屏下电容键（raw_x≈0x03a0, raw_y≈0x1020 sentinel）与普通触控互斥；
// 输出带时间戳的轨迹、tap/hold/swipe 与 Front short/long/double。
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

#include <atomic>
#include <vector>

namespace eego {

class Gslx680Touch {
public:
    Gslx680Touch();
    ~Gslx680Touch();

    esp_err_t begin(i2c_port_t port = I2C_NUM_0);  // 初始化 I²C + 上传固件
    void end();

    // 启动后台采样任务（需 begin() 成功）。wake 非空时每批事件入队后 give 一次，
    // 供消费者阻塞等待。I²C 访问经 legacy 驱动的端口级互斥，与 RTC/前光安全共享。
    esp_err_t startPolling(SemaphoreHandle_t wake = nullptr);
    // 请求后台任务退出并等待其真正结束（不能外部删除：它可能持有 I²C 锁）。
    // 返回 false 表示等待超时、任务仍存活（此时调用方不得重置采样状态）。
    bool stopPolling();
    bool pollingActive() const { return queue_ != nullptr && running_.load(); }

    // 取一个事件（非阻塞）；无事件返回 type=None。任务模式下读队列，
    // 否则同步读一帧（旧行为，需调用者约每 10ms 调用）。
    TouchEvent poll();

    bool isPressed() const { return pressed_.load(); }
    bool hasPending() const;
    uint32_t droppedEvents() const { return dropped_.load(); }

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
    bool initialized_ = false;  // I²C 总线已安装
    bool enabled_ = false;      // 固件上传并校验通过
    TouchGestureFsm gestures_;            // 任务模式下仅由采样任务访问
    std::vector<TouchEvent> pending_;     // 仅同步模式使用
    QueueHandle_t queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> taskExited_{true};
    std::atomic<bool> pressed_{false};
    std::atomic<uint32_t> dropped_{0};
    SemaphoreHandle_t wake_ = nullptr;
};

}  // namespace eego
