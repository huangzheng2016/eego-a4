// SPDX-License-Identifier: GPL-3.0-or-later
// 移植自 eego-a4-weread（GPL-3.0）同名文件；原始硬件事实来自 eego-a4-template-firmware（MIT）。
// EEGO A4 按键管理实现
// 三键: GPIO5(Up, active-low), GPIO7(Down, active-low), GPIO8(Power, active-high)
// 5ms 去抖 + 时间戳边沿/短按/长按/双击/chord + 唤醒释放门控
#include "buttons.h"
#include "eego_pins.h"

#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#include "input_gesture.h"

static const char* BTN_TAG = "eego_btn";

// 轮询周期
static constexpr uint32_t POLL_PERIOD_MS = 15;
// 去抖延时
static constexpr uint32_t DEBOUNCE_MS = 5;
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

void ButtonManager::pollTask(void* arg) {
    ButtonManager* self = static_cast<ButtonManager*>(arg);
    ButtonGestureFsm gestures;
    uint8_t initialMask = 0;
    for (size_t i = 0; i < kButtonCount; i++) {
        const int level = gpio_get_level(static_cast<gpio_num_t>(kButtons[i].pin));
        const bool pressed = kButtons[i].activeHigh ? level == 1 : level == 0;
        if (pressed) initialMask |= static_cast<uint8_t>(1u << i);
    }
    gestures.reset(esp_timer_get_time(), initialMask,
                   esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0);

    while (self->running_) {
        bool sample1[kButtonCount];
        for (size_t i = 0; i < kButtonCount; i++) {
            const int level = gpio_get_level(static_cast<gpio_num_t>(kButtons[i].pin));
            sample1[i] = kButtons[i].activeHigh ? level == 1 : level == 0;
        }
        vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));

        bool sample2[kButtonCount];
        for (size_t i = 0; i < kButtonCount; i++) {
            const int level = gpio_get_level(static_cast<gpio_num_t>(kButtons[i].pin));
            sample2[i] = kButtons[i].activeHigh ? level == 1 : level == 0;
        }
        const int64_t now = esp_timer_get_time();
        for (size_t i = 0; i < kButtonCount; i++) {
            if (sample1[i] != sample2[i]) continue;
            const auto events = gestures.sample(kButtons[i].id, sample2[i], now);
            for (const ButtonEvent& event : events) {
                xQueueSend(self->queue_, &event, 0);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }

    self->task_ = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace eego
