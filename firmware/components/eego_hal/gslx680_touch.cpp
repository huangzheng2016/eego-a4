// SPDX-License-Identifier: GPL-3.0-or-later
// 移植自 eego-a4-weread（GPL-3.0）同名文件；原始硬件事实来自 eego-a4-template-firmware（MIT）。
// GSLX680 电容触控驱动实现（EEGO A4）— 纯 ESP-IDF
//
// 协议说明：EEGO A4 的 GSLX680 固件表为 {页内 offset, 32 位 value} 结构，
// 上传时通过 0xf0 寄存器选页，再将页内连续寄存器（offset 递增 4）批量写入。
// 这与通用 GSL1680 的 0xbc/0xbd/0xbe 单字节流式上传不同，是本设备的实际协议，
// 移植自 eego-a4-template-ref/lib/InputManager 的 gslx680* 实现。
#include "gslx680_touch.h"
#include "gslx680_firmware.h"
#include "touch_calibration_store.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

namespace eego {

namespace {
constexpr uint32_t kTouchPollPeriodMs = 10;   // 采样周期（100Hz tick 下 = 1 tick）
constexpr UBaseType_t kTouchQueueLen = 64;
constexpr UBaseType_t kMoveReserveSlots = 16;  // 队列余量不足时丢弃 Move，保留关键事件
constexpr uint32_t kTouchTaskStack = 4096;
constexpr UBaseType_t kTouchTaskPriority = 3;  // 高于 app_task(1)/按键(2)，I²C 读 <1ms

// 最少 1 个 tick 的毫秒延时（兼容 100Hz tick 配置下 pdMS_TO_TICKS 小于 1 的情况）
void delayMs(uint32_t ms) {
    TickType_t ticks = pdMS_TO_TICKS(ms);
    if (ticks == 0) ticks = 1;
    vTaskDelay(ticks);
}
}  // namespace

Gslx680Touch::Gslx680Touch() {}

Gslx680Touch::~Gslx680Touch() { end(); }

// --- I²C 原语 ---

esp_err_t Gslx680Touch::writeReg(uint8_t reg, const uint8_t* data, size_t len) {
    if (len == 0 || data == nullptr) {
        return i2c_master_write_to_device(i2cPort_, GSLX680_ADDRESS, &reg, 1,
                                          pdMS_TO_TICKS(100));
    }
    // 固件批量写入最大 64 字节 + 1 寄存器字节
    uint8_t buf[65];
    buf[0] = reg;
    memcpy(buf + 1, data, len);
    return i2c_master_write_to_device(i2cPort_, GSLX680_ADDRESS, buf, len + 1,
                                      pdMS_TO_TICKS(100));
}

esp_err_t Gslx680Touch::writeReg32(uint8_t reg, uint32_t value) {
    uint8_t data[4] = {
        static_cast<uint8_t>(value),
        static_cast<uint8_t>(value >> 8),
        static_cast<uint8_t>(value >> 16),
        static_cast<uint8_t>(value >> 24),
    };
    return writeReg(reg, data, 4);
}

esp_err_t Gslx680Touch::readReg(uint8_t reg, uint8_t* data, size_t len) {
    return i2c_master_write_read_device(i2cPort_, GSLX680_ADDRESS, &reg, 1, data,
                                        len, pdMS_TO_TICKS(100));
}

// --- 芯片控制（移植自参考代码 gslx680ClearRegisters/ResetChip/StartChip/Check）---

void Gslx680Touch::clearRegisters() {
    writeReg32(0xe0, 0x00000088);
    delayMs(20);
    writeReg32(0x80, 0x00000003);
    delayMs(5);
    writeReg32(0xe4, 0x00000004);
    delayMs(5);
    writeReg32(0xe0, 0x00000000);
    delayMs(20);
}

void Gslx680Touch::resetChip() {
    writeReg32(0xe0, 0x00000088);
    delayMs(20);
    writeReg32(0xe4, 0x00000004);
    delayMs(10);
    writeReg32(0xbc, 0x00000000);
    delayMs(10);
}

void Gslx680Touch::startChip() {
    writeReg32(0xe0, 0x00000000);
    delayMs(10);
}

bool Gslx680Touch::checkChip() {
    uint8_t status[4] = {};
    if (readReg(0xb0, status, sizeof(status)) != ESP_OK) return false;
    return status[0] == 0x5a && status[1] == 0x5a && status[2] == 0x5a &&
           status[3] == 0x5a;
}

// --- 固件上传 ---
// offset==0xf0：页选择，直接写 32 位值到 0xf0。
// 其余：将最多 16 个连续 32 位寄存器（offset 递增 4）合并为一次 I²C 写入，
//      起始寄存器为首个 offset。参考代码的 OEM 批量优化。

esp_err_t Gslx680Touch::uploadFirmware() {
    size_t i = 0;
    while (i < GSLX680_FIRMWARE_COUNT) {
        const Gslx680FirmwareEntry& entry = GSLX680_FIRMWARE[i];
        if (entry.offset == 0xf0) {
            if (writeReg32(entry.offset, entry.value) != ESP_OK) return ESP_FAIL;
            ++i;
            continue;
        }

        uint8_t payload[64];
        uint8_t words = 0;
        const uint8_t firstReg = entry.offset;
        while (i < GSLX680_FIRMWARE_COUNT && words < 16) {
            const Gslx680FirmwareEntry& e = GSLX680_FIRMWARE[i];
            const uint8_t expected = static_cast<uint8_t>(firstReg + words * 4);
            if (e.offset == 0xf0 || e.offset != expected) break;
            payload[words * 4] = static_cast<uint8_t>(e.value);
            payload[words * 4 + 1] = static_cast<uint8_t>(e.value >> 8);
            payload[words * 4 + 2] = static_cast<uint8_t>(e.value >> 16);
            payload[words * 4 + 3] = static_cast<uint8_t>(e.value >> 24);
            ++words;
            ++i;
        }
        if (words == 0) {
            ++i;  // 安全跳过非预期条目
            continue;
        }
        if (writeReg(firstReg, payload, static_cast<size_t>(words) * 4) != ESP_OK)
            return ESP_FAIL;
    }
    return ESP_OK;
}

// --- 生命周期 ---

esp_err_t Gslx680Touch::begin(i2c_port_t port) {
    i2cPort_ = port;

    // 配置共享 I²C 总线（SDA=2, SCL=1, 400kHz）
    i2c_config_t conf;
    memset(&conf, 0, sizeof(conf));
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = static_cast<gpio_num_t>(I2C_SDA);
    conf.scl_io_num = static_cast<gpio_num_t>(I2C_SCL);
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = I2C_HZ;
    esp_err_t err = i2c_param_config(i2cPort_, &conf);
    if (err != ESP_OK) return err;

    // 安装驱动；若已安装（与 PCF8563 共享 I2C_NUM_0）则视为成功
    err = i2c_driver_install(i2cPort_, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    initialized_ = true;

    // 探测并演练页选择寄存器（与 CrossLink 一致）
    uint8_t probe = 0;
    if (readReg(0xf0, &probe, 1) != ESP_OK ||
        writeReg32(0xf0, 0x00000012) != ESP_OK ||
        readReg(0xf0, &probe, 1) != ESP_OK) {
        enabled_ = false;
        return ESP_FAIL;
    }

    // 复位/清寄存器序列
    clearRegisters();
    resetChip();
    resetChip();
    clearRegisters();
    resetChip();

    // 上传固件并启动
    if (uploadFirmware() != ESP_OK) {
        enabled_ = false;
        return ESP_FAIL;
    }
    startChip();
    resetChip();
    startChip();

    // 校验：0xb0 应返回 0x5a5a5a5a
    if (!checkChip()) {
        resetChip();
        startChip();
    }
    enabled_ = checkChip();
    if (enabled_) {
        touchcal::Affine saved;
        if (touchcal::loadPersisted(saved) == ESP_OK) {
            applyCalibration(saved);
            ESP_LOGI("gslx680", "touch calibration loaded from NVS");
        }
    }
    return enabled_ ? ESP_OK : ESP_FAIL;
}

void Gslx680Touch::end() {
    enabled_ = false;
    // 任务仍存活时它还在使用 gestures_/pressed_：不得重置，留给任务退出后。
    if (!stopPolling()) return;
    gestures_.reset();
    pending_.clear();
    pressed_.store(false);
    // 不卸载共享 I²C 总线（与 PCF8563 共用 I2C_NUM_0）
}

// --- 采样 ---
// 读 0x80 的 24 字节帧：
//   frame[0] = 触点数；point0 的 raw_y 在 bytes[4-5]、raw_x 在 bytes[6-7]（小端）。
// 屏下电容键 sentinel：raw_x==0x03a0 且 raw_y==0x1020。

std::vector<TouchEvent> Gslx680Touch::sampleFrame() {
    const int64_t now = esp_timer_get_time();
    uint8_t frame[24] = {};
    std::vector<TouchEvent> events;
    bool screenFrame = false;
    uint16_t rawX = 0;
    uint16_t rawY = 0;
    if (readReg(0x80, frame, sizeof(frame)) != ESP_OK) {
        events = gestures_.invalidate(now);
    } else {
        const uint8_t count = frame[0] > 5 ? 5 : frame[0];
        const uint16_t rawYWord =
            (static_cast<uint16_t>(frame[5]) << 8) | frame[4];
        const uint16_t rawXWord =
            (static_cast<uint16_t>(frame[7]) << 8) | frame[6];
        const bool homeKey = rawXWord == FRONT_KEY_RAW_X && rawYWord == FRONT_KEY_RAW_Y;
        if (count == 0) {
            events = gestures_.sample(TouchContact::None, 0, 0, now);
        } else if (homeKey) {
            events = gestures_.sample(TouchContact::Front, 0, 0, now);
        } else {
            rawY = rawYWord & 0x0fff;
            rawX = rawXWord & 0x0fff;
            screenFrame = true;
            if (!gestures_.screenPressed()) {  // 新的屏幕接触：重置原始坐标累计
                rawSumX_ = rawSumY_ = 0;
                rawCount_ = 0;
            }
            if (rawCount_ < 0xffff) {
                rawSumX_ += rawX;
                rawSumY_ += rawY;
                ++rawCount_;
            }
            uint16_t x = 0;
            uint16_t y = 0;
            touchcal::map(calibration(), rawX, rawY, x, y);
            events = gestures_.sample(TouchContact::Screen, x, y, now);
        }
    }
    // 附带原始坐标：Press/Move 用本帧值，接触结束类事件用本次接触的均值。
    for (TouchEvent& ev : events) {
        if (screenFrame &&
            (ev.type == TouchEventType::Press || ev.type == TouchEventType::Move)) {
            ev.raw_x = rawX;
            ev.raw_y = rawY;
            ev.raw_samples = 1;
        } else if (rawCount_ > 0 &&
                   (ev.type == TouchEventType::Release || ev.type == TouchEventType::Tap ||
                    ev.type == TouchEventType::Hold || ev.type == TouchEventType::Cancel ||
                    ev.type == TouchEventType::SwipeLeft ||
                    ev.type == TouchEventType::SwipeRight ||
                    ev.type == TouchEventType::SwipeUp ||
                    ev.type == TouchEventType::SwipeDown)) {
            ev.raw_x = static_cast<uint16_t>((rawSumX_ + rawCount_ / 2) / rawCount_);
            ev.raw_y = static_cast<uint16_t>((rawSumY_ + rawCount_ / 2) / rawCount_);
            ev.raw_samples = rawCount_;
        }
    }
    pressed_.store(gestures_.screenPressed());
    return events;
}

// --- 仿射标定 ---

touchcal::Affine Gslx680Touch::calibration() const {
    std::lock_guard<std::mutex> lock(calMutex_);
    return cal_;
}

bool Gslx680Touch::calibrationCustom() const { return !touchcal::isDefault(calibration()); }

bool Gslx680Touch::applyCalibration(const touchcal::Affine& affine) {
    if (!touchcal::plausible(affine)) return false;
    std::lock_guard<std::mutex> lock(calMutex_);
    cal_ = affine;
    return true;
}

esp_err_t Gslx680Touch::saveCalibration(const touchcal::Affine& affine) {
    if (!touchcal::plausible(affine)) return ESP_ERR_INVALID_ARG;
    const esp_err_t err = touchcal::savePersisted(affine);
    if (err != ESP_OK) return err;
    applyCalibration(affine);
    return ESP_OK;
}

esp_err_t Gslx680Touch::clearCalibration() {
    applyCalibration(touchcal::defaultAffine());
    return touchcal::erasePersisted();
}

// --- 后台采样任务 ---

esp_err_t Gslx680Touch::startPolling(SemaphoreHandle_t wake) {
    if (!enabled_) return ESP_ERR_INVALID_STATE;
    if (running_.load()) return ESP_OK;
    if (!taskExited_.load()) return ESP_ERR_INVALID_STATE;  // 上一个任务尚未退出
    wake_ = wake;
    if (queue_ == nullptr) {
        queue_ = xQueueCreate(kTouchQueueLen, sizeof(TouchEvent));
        if (queue_ == nullptr) return ESP_ERR_NO_MEM;
    }
    pending_.clear();  // 同步模式残留事件不再交付
    running_.store(true);
    taskExited_.store(false);
    if (xTaskCreate(&Gslx680Touch::pollTask, "eego_touch", kTouchTaskStack, this,
                    kTouchTaskPriority, &task_) != pdPASS) {
        running_.store(false);
        taskExited_.store(true);
        task_ = nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool Gslx680Touch::stopPolling() {
    if (taskExited_.load()) {
        running_.store(false);
        return true;
    }
    running_.store(false);
    // 让任务自行退出（不可外部删除：它可能正持有 I²C 端口锁，或阻塞在与
    // RTC/前光共享的 I²C 事务上）。等到 taskExited_ 置位，上限 3 s。
    for (int i = 0; i < 300 && !taskExited_.load(); ++i) delayMs(10);
    if (!taskExited_.load()) {
        ESP_LOGE("gslx680", "touch task did not exit; sampling state left untouched");
        return false;
    }
    return true;
}

void Gslx680Touch::pollTask(void* arg) {
    Gslx680Touch* self = static_cast<Gslx680Touch*>(arg);
    TickType_t period = pdMS_TO_TICKS(kTouchPollPeriodMs);
    if (period == 0) period = 1;
    TickType_t lastWake = xTaskGetTickCount();
    while (self->running_.load()) {
        const std::vector<TouchEvent> events = self->sampleFrame();
        bool sent = false;
        for (const TouchEvent& ev : events) {
            if (ev.type == TouchEventType::Move &&
                uxQueueSpacesAvailable(self->queue_) < kMoveReserveSlots) {
                self->dropped_.fetch_add(1);  // 消费者落后：Move 可合并丢弃
                continue;
            }
            if (xQueueSend(self->queue_, &ev, 0) == pdTRUE) {
                sent = true;
            } else {
                self->dropped_.fetch_add(1);
            }
        }
        if (sent && self->wake_ != nullptr) xSemaphoreGive(self->wake_);
        vTaskDelayUntil(&lastWake, period);
    }
    self->task_ = nullptr;
    self->taskExited_.store(true);
    vTaskDelete(nullptr);
}

// --- 消费者接口 ---

bool Gslx680Touch::hasPending() const {
    if (pollingActive() || (!taskExited_.load() && queue_ != nullptr))
        return uxQueueMessagesWaiting(queue_) > 0;
    return !pending_.empty();
}

TouchEvent Gslx680Touch::poll() {
    TouchEvent none;
    if (pollingActive()) {
        TouchEvent ev;
        if (xQueueReceive(queue_, &ev, 0) == pdTRUE) return ev;
        return none;
    }

    // 任务已被要求停止但尚未退出：不能同步采样（与任务并发访问手势 FSM/I²C）。
    // 只交付任务已排队的事件。
    if (!taskExited_.load()) {
        TouchEvent ev;
        if (queue_ != nullptr && xQueueReceive(queue_, &ev, 0) == pdTRUE) return ev;
        return none;
    }

    if (!pending_.empty()) {
        const TouchEvent out = pending_.front();
        pending_.erase(pending_.begin());
        return out;
    }
    if (!enabled_) return none;
    std::vector<TouchEvent> events = sampleFrame();
    if (events.empty()) return none;
    pending_ = std::move(events);
    const TouchEvent out = pending_.front();
    pending_.erase(pending_.begin());
    return out;
}

}  // namespace eego
