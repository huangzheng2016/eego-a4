// SPDX-License-Identifier: GPL-3.0-or-later
// 移植自 eego-a4-weread（GPL-3.0）同名文件；原始硬件事实来自 eego-a4-template-firmware（MIT）。
// EEGO A4 按键管理实现
// 三键: GPIO5(Up, active-low), GPIO7(Down, active-low), GPIO8(Power, active-high)
// 连续采样去抖 + 时间戳边沿/短按/长按/可选双击/chord + 唤醒释放门控
#include "buttons.h"
#include "eego_pins.h"

#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#include "input_gesture.h"

static const char* BTN_TAG = "eego_btn";

// 轮询周期（100Hz tick 下取整为 1 tick = 10ms；至少 1 tick）
static constexpr uint32_t POLL_PERIOD_MS = 10;
// 去抖：原始电平需连续 N 次采样一致（与当前稳定态不同）才接受边沿。
// 2 次 × 10ms 轮询 ≈ 10-20ms 去抖窗口。
static constexpr uint8_t DEBOUNCE_SAMPLES = 2;
// 原始边沿与手势共用队列，保留双击/chord 的额外事件空间。
static constexpr uint32_t QUEUE_LEN = 64;
// 任务栈大小
static constexpr uint32_t TASK_STACK = 4096;
// 任务优先级（低于显示刷新）
static constexpr UBaseType_t TASK_PRIORITY = 2;

namespace eego {

// 按键配置表
struct BtnCfg {
    ButtonId id;
    int pin;
    bool activeHigh;
};

static const BtnCfg kButtons[] = {
    {ButtonId::Up,    BUTTON_UP,    false},  // active-low
    {ButtonId::Down,  BUTTON_DOWN,  false},  // active-low
    {ButtonId::Power, BUTTON_POWER, true},   // active-high
};
static constexpr size_t kButtonCount = sizeof(kButtons) / sizeof(kButtons[0]);

ButtonManager& ButtonManager::instance() {
    static ButtonManager inst;
    return inst;
}

esp_err_t ButtonManager::begin() {
    if (running_) return ESP_OK;  // 已启动

    // 配置 Up/Down 引脚：active-low → INPUT_PULLUP
    gpio_config_t pullupConf;
    pullupConf.pin_bit_mask = (1ULL << BUTTON_UP) | (1ULL << BUTTON_DOWN);
    pullupConf.mode = GPIO_MODE_INPUT;
    pullupConf.pull_up_en = GPIO_PULLUP_ENABLE;
    pullupConf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    pullupConf.intr_type = GPIO_INTR_DISABLE;
    esp_err_t err = gpio_config(&pullupConf);
    if (err != ESP_OK) {
        ESP_LOGE(BTN_TAG, "Up/Down GPIO config failed: %s", esp_err_to_name(err));
        return err;
    }

    // 配置 Power 引脚：active-high → INPUT_PULLDOWN
    gpio_config_t pulldownConf;
    pulldownConf.pin_bit_mask = (1ULL << BUTTON_POWER);
    pulldownConf.mode = GPIO_MODE_INPUT;
    pulldownConf.pull_up_en = GPIO_PULLUP_DISABLE;
    pulldownConf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    pulldownConf.intr_type = GPIO_INTR_DISABLE;
    err = gpio_config(&pulldownConf);
    if (err != ESP_OK) {
        ESP_LOGE(BTN_TAG, "Power GPIO config failed: %s", esp_err_to_name(err));
        return err;
    }

    // 创建事件队列
    queue_ = xQueueCreate(QUEUE_LEN, sizeof(ButtonEvent));
    if (!queue_) {
        ESP_LOGE(BTN_TAG, "Queue creation failed");
        return ESP_ERR_NO_MEM;
    }

    // 启动后台轮询任务
    running_ = true;
    BaseType_t ret = xTaskCreate(pollTask, "eego_btn", TASK_STACK, this,
                                  TASK_PRIORITY, &task_);
    if (ret != pdPASS) {
        ESP_LOGE(BTN_TAG, "Task creation failed");
        running_ = false;
        vQueueDelete(queue_);
        queue_ = nullptr;
        return ESP_FAIL;
    }

    ESP_LOGI(BTN_TAG, "Button manager started (3 buttons, %ums poll)", POLL_PERIOD_MS);
    return ESP_OK;
}

void ButtonManager::end() {
    running_ = false;
    if (task_) {
        TaskHandle_t t = task_;
        task_ = nullptr;
        // 从外部删除任务（FreeRTOS 会释放栈和 TCB）
        vTaskDelete(t);
    }
    if (queue_) {
        vQueueDelete(queue_);
        queue_ = nullptr;
    }
}

bool ButtonManager::poll(ButtonEvent* event, uint32_t timeoutMs) {
    if (!queue_ || !event) return false;
    return xQueueReceive(queue_, event, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void ButtonManager::setDoublePressEnabled(ButtonId id, bool enabled) {
    if (id == ButtonId::Power) return;
    const uint8_t bit = static_cast<uint8_t>(1u << static_cast<uint8_t>(id));
    if (enabled) {
        doubleMask_.fetch_or(bit);
    } else {
        doubleMask_.fetch_and(static_cast<uint8_t>(~bit));
    }
}

bool ButtonManager::doublePressEnabled(ButtonId id) const {
    if (id == ButtonId::Power) return false;
    return (doubleMask_.load() & (1u << static_cast<uint8_t>(id))) != 0;
}

static bool readPressed(size_t i) {
    const int level = gpio_get_level(static_cast<gpio_num_t>(kButtons[i].pin));
    return kButtons[i].activeHigh ? level == 1 : level == 0;
}

void ButtonManager::pollTask(void* arg) {
    ButtonManager* self = static_cast<ButtonManager*>(arg);
    ButtonGestureFsm gestures;
    uint8_t initialMask = 0;
    bool stable[kButtonCount];
    uint8_t diffCount[kButtonCount] = {};
    for (size_t i = 0; i < kButtonCount; i++) {
        stable[i] = readPressed(i);
        if (stable[i]) initialMask |= static_cast<uint8_t>(1u << i);
    }
    gestures.reset(esp_timer_get_time(), initialMask,
                   esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0);

    TickType_t period = pdMS_TO_TICKS(POLL_PERIOD_MS);
    if (period == 0) period = 1;
    TickType_t lastWake = xTaskGetTickCount();
    uint8_t appliedDoubleMask = 0xff;  // 强制首轮同步

    while (self->running_.load()) {
        const uint8_t doubleMask = self->doubleMask_.load();
        if (doubleMask != appliedDoubleMask) {
            for (size_t i = 0; i < kButtonCount; i++) {
                gestures.setDoublePressEnabled(kButtons[i].id,
                                               (doubleMask & (1u << i)) != 0);
            }
            appliedDoubleMask = doubleMask;
        }

        const int64_t now = esp_timer_get_time();
        bool sent = false;
        for (size_t i = 0; i < kButtonCount; i++) {
            const bool raw = readPressed(i);
            if (raw != stable[i]) {
                if (++diffCount[i] >= DEBOUNCE_SAMPLES) {
                    stable[i] = raw;
                    diffCount[i] = 0;
                }
            } else {
                diffCount[i] = 0;
            }
            // 每轮都喂 FSM（稳定态不变时仅推进长按/短按超时）
            const auto events = gestures.sample(kButtons[i].id, stable[i], now);
            for (const ButtonEvent& event : events) {
                if (xQueueSend(self->queue_, &event, 0) == pdTRUE) sent = true;
            }
        }
        if (sent) {
            SemaphoreHandle_t wake = self->wake_.load();
            if (wake != nullptr) xSemaphoreGive(wake);
        }
        vTaskDelayUntil(&lastWake, period);
    }

    self->task_ = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace eego
