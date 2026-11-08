// SPDX-License-Identifier: GPL-3.0-or-later

#include "uc8279c_display.h"
#include "uc8279c_luts.h"

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_psram.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstring>

static const char* TAG_UC = "eego.display";

namespace eego {
namespace {
std::atomic<int> g_refreshDepth{0};
struct RefreshScope {
    RefreshScope() { g_refreshDepth.fetch_add(1, std::memory_order_relaxed); }
    ~RefreshScope() { g_refreshDepth.fetch_sub(1, std::memory_order_relaxed); }
};

constexpr uint8_t CMD_PANEL_SETTING = 0x00;
constexpr uint8_t CMD_POWER_SETTING = 0x01;
constexpr uint8_t CMD_POWER_OFF = 0x02;
constexpr uint8_t CMD_POWER_OFF_SEQUENCE = 0x03;
constexpr uint8_t CMD_POWER_ON = 0x04;
constexpr uint8_t CMD_BOOSTER_SOFT_START = 0x06;
constexpr uint8_t CMD_DEEP_SLEEP = 0x07;
constexpr uint8_t CMD_DTM1 = 0x10;
constexpr uint8_t CMD_DISPLAY_REFRESH = 0x12;
constexpr uint8_t CMD_DTM2 = 0x13;
constexpr uint8_t CMD_LUT_VCOM = 0x20;
constexpr uint8_t CMD_PLL_CONTROL = 0x30;
constexpr uint8_t CMD_VCOM_DATA_INTERVAL = 0x50;
constexpr uint8_t CMD_RESOLUTION = 0x61;
constexpr uint8_t CMD_GATE_SOURCE_SETTING = 0x65;
constexpr uint8_t CMD_VCOM_DC = 0x82;
constexpr uint8_t CMD_TCON = 0xe1;

constexpr int64_t BUSY_TIMEOUT_US = 30 * 1000 * 1000LL;
constexpr int64_t BUSY_SETTLE_US = 100 * 1000LL;
constexpr int64_t BUSY_START_CONFIRM_US = 200 * 1000LL;
constexpr int64_t BUSY_IDLE_CONFIRM_MS = 30;

inline gpio_num_t gn(int8_t pin) {
    return static_cast<gpio_num_t>(pin);
}

}  // namespace

Uc8279cDisplay::Uc8279cDisplay() = default;

Uc8279cDisplay::~Uc8279cDisplay() {
    end();
}

esp_err_t Uc8279cDisplay::beginOffscreen() {
    if (spi_ != nullptr) return ESP_ERR_INVALID_STATE;
    offscreen_ = true;
    if (!fb_) {
        fb_ = static_cast<uint8_t*>(heap_caps_malloc(FRAMEBUFFER_SIZE, MALLOC_CAP_SPIRAM));
        if (!fb_) return ESP_ERR_NO_MEM;
    }
    clear();
    return ESP_OK;
}

esp_err_t Uc8279cDisplay::begin(bool deferController) {
    if (offscreen_) return ESP_ERR_INVALID_STATE;
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psram_total = esp_psram_get_size();
    ESP_LOGI(TAG_UC, "PSRAM total=%u free=%u, FB_SIZE=%u",
             (unsigned)psram_total, (unsigned)psram_free, (unsigned)FRAMEBUFFER_SIZE);

    if (!fb_) {
        fb_ = static_cast<uint8_t*>(heap_caps_malloc(FRAMEBUFFER_SIZE, MALLOC_CAP_SPIRAM));
        if (!fb_) {
            ESP_LOGE(TAG_UC, "PSRAM 分配失败! 尝试内部 RAM");
            fb_ = static_cast<uint8_t*>(heap_caps_malloc(FRAMEBUFFER_SIZE, MALLOC_CAP_INTERNAL));
            if (!fb_) {
                ESP_LOGE(TAG_UC, "内部 RAM 也分配失败!");
                return ESP_ERR_NO_MEM;
            }
        }
        ESP_LOGI(TAG_UC, "framebuffer @ %p (%u 字节)", fb_, (unsigned)FRAMEBUFFER_SIZE);
        clear();
    }

    gpioInit();
    esp_err_t ret = spiInit();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_UC, "SPI 初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG_UC, "spi_ handle = %p", spi_);

    if (deferController) {
        controllerPending_ = true;
        fastRefreshesSinceFull_ = 0;
        return ESP_OK;
    }
    powerUpController();
    return ESP_OK;
}

void Uc8279cDisplay::powerUpController() {
    gpio_set_level(gn(EPD_POWER_ENABLE), 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    hardwareReset();

    initController();
    fastRefreshesSinceFull_ = 0;
    controllerPending_ = false;
}

void Uc8279cDisplay::ensureController() {
    if (controllerPending_) powerUpController();
}

void Uc8279cDisplay::end() {
    if (fb_) {
        free(fb_);
        fb_ = nullptr;
    }
    if (fbGrayMsb_) {
        free(fbGrayMsb_);
        fbGrayMsb_ = nullptr;
    }
    if (fbGrayLsb_) {
        free(fbGrayLsb_);
        fbGrayLsb_ = nullptr;
    }
    planesActive_ = false;
    if (offscreen_) return;
    if (spi_) {
        spi_bus_remove_device(spi_);
        spi_ = nullptr;
    }
    spi_bus_free(SPI3_HOST);
    screenOn_ = false;
    grayControllerMode_ = false;
    fastRefreshesSinceFull_ = 0;
}

void Uc8279cDisplay::gpioInit() {
    gpio_hold_dis(gn(EPD_POWER_ENABLE));

    gpio_set_direction(gn(EPD_POWER_ENABLE), GPIO_MODE_OUTPUT);
    gpio_set_direction(gn(EPD_CS), GPIO_MODE_OUTPUT);
    gpio_set_direction(gn(EPD_DC), GPIO_MODE_OUTPUT);
    gpio_set_direction(gn(EPD_RESET), GPIO_MODE_OUTPUT);

    gpio_set_direction(gn(EPD_BUSY), GPIO_MODE_INPUT);
    gpio_pullup_en(gn(EPD_BUSY));

    gpio_set_level(gn(EPD_POWER_ENABLE), 0);
    gpio_set_level(gn(EPD_CS), 1);
    gpio_set_level(gn(EPD_DC), 1);
    gpio_set_level(gn(EPD_RESET), 1);
}

esp_err_t Uc8279cDisplay::spiInit() {
    if (spi_) {
        ESP_LOGI(TAG_UC, "SPI 已初始化，跳过（handle=%p）", spi_);
        return ESP_OK;
    }
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = EPD_MOSI;
    buscfg.miso_io_num = -1;
    buscfg.sclk_io_num = EPD_SCLK;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = 4096;

    spi_device_interface_config_t devcfg = {};
    devcfg.clock_speed_hz = static_cast<int>(EPD_SPI_HZ);  // 20MHz
    devcfg.mode = 0;                                         // SPI MODE 0
    devcfg.spics_io_num = -1;
    devcfg.queue_size = 1;

    ESP_LOGI(TAG_UC, "SPI init: host=SPI3 SCLK=%d MOSI=%d CS=%d DC=%d RST=%d BUSY=%d",
             EPD_SCLK, EPD_MOSI, EPD_CS, EPD_DC, EPD_RESET, EPD_BUSY);

    esp_err_t ret = spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_UC, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = spi_bus_add_device(SPI3_HOST, &devcfg, &spi_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_UC, "spi_bus_add_device failed: %s", esp_err_to_name(ret));
        spi_bus_free(SPI3_HOST);
        return ret;
    }
    ESP_LOGI(TAG_UC, "SPI init OK, handle=%p", spi_);
    return ESP_OK;
}

void Uc8279cDisplay::hardwareReset() {
    // RST: HIGH 20ms → LOW 10ms → HIGH 100ms
    gpio_set_level(gn(EPD_RESET), 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(gn(EPD_RESET), 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(gn(EPD_RESET), 1);
    vTaskDelay(pdMS_TO_TICKS(100));
}

void Uc8279cDisplay::initController(bool grayMode) {
    const uint8_t panel[] = {0x3f, 0x4a};
    const uint8_t power[] = {static_cast<uint8_t>(grayMode ? 0x03 : 0x43), 0x00, 0x78, 0x78, 0x17};
    const uint8_t booster[] = {0x25, 0x25, 0x3c};
    const uint8_t resolution[] = {0x03, 0x00, 0x02, 0x58};  // 768×600
    const uint8_t interval[] = {0x00, 0x00, 0x20, 0x00};
    const uint8_t powerOffSequence = 0x20;
    const uint8_t vcom = grayMode ? 0x20 : 0x24;
    const uint8_t pll = 0x0f;
    const uint8_t tcon = 0x02;

    cmdData(CMD_PANEL_SETTING, panel, sizeof(panel));
    cmdData(CMD_POWER_OFF_SEQUENCE, &powerOffSequence, 1);
    cmdData(CMD_POWER_SETTING, power, sizeof(power));
    cmdData(CMD_BOOSTER_SOFT_START, booster, sizeof(booster));
    cmdData(CMD_VCOM_DC, &vcom, 1);
    cmdData(CMD_PLL_CONTROL, &pll, 1);
    cmdData(CMD_RESOLUTION, resolution, sizeof(resolution));
    cmdData(CMD_GATE_SOURCE_SETTING, interval, sizeof(interval));
    cmdData(CMD_TCON, &tcon, 1);

    if (grayMode) {
        sendCmd(CMD_POWER_ON);
        waitBusy("gray power-on");
        fillControllerRam(CMD_DTM1, 0xff);
    } else {
        fillControllerRam(CMD_DTM1, 0xff);
        sendCmd(CMD_POWER_ON);
        waitBusy("power-on");
    }
    screenOn_ = true;
    grayControllerMode_ = grayMode;
}

void Uc8279cDisplay::ensurePowerOn() {
    if (screenOn_) return;
    sendCmd(CMD_POWER_ON);
    waitBusy("power-on");
    screenOn_ = true;
}

void Uc8279cDisplay::fillControllerRam(uint8_t cmd, uint8_t fill) {
    uint8_t row[PANEL_ROW_BYTES];
    memset(row, fill, sizeof(row));

    sendCmd(cmd);

    gpio_set_level(gn(EPD_CS), 0);
    gpio_set_level(gn(EPD_DC), 1);
    for (uint16_t y = 0; y < CONTROLLER_HEIGHT; ++y) {
        spiTransmit(row, PANEL_ROW_BYTES);
    }
    gpio_set_level(gn(EPD_CS), 1);
}

void Uc8279cDisplay::loadFullLut() {
    const uint8_t* const lut[] = {
        A4_UC8279C_LUT_20, A4_UC8279C_LUT_21, A4_UC8279C_LUT_22,
        A4_UC8279C_LUT_23, A4_UC8279C_LUT_24};
    for (uint8_t i = 0; i < 5; ++i) {
        cmdData(static_cast<uint8_t>(CMD_LUT_VCOM + i), lut[i], A4_UC8279C_LUT_LENGTH);
    }
}

void Uc8279cDisplay::loadFastLut() {
    for (uint8_t i = 0; i < 5; ++i) {
        cmdData(static_cast<uint8_t>(CMD_LUT_VCOM + i),
                A4_UC8279C_FAST_LUT + i * A4_UC8279C_FAST_RECORD_LENGTH,
                A4_UC8279C_FAST_WRITE_LENGTH);
    }
}

void Uc8279cDisplay::loadGrayLut() {
    for (uint8_t i = 0; i < 5; ++i) {
        cmdData(static_cast<uint8_t>(CMD_LUT_VCOM + i),
                A4_UC8279C_GRAY_LUT + i * A4_UC8279C_GRAY_RECORD_LENGTH,
                A4_UC8279C_GRAY_RECORD_LENGTH);
    }
}

void Uc8279cDisplay::writeFrame(uint8_t cmd, const uint8_t* fb) {
    uint8_t white[PANEL_ROW_BYTES];
    memset(white, 0xff, sizeof(white));
    // The framebuffer lives in PSRAM; copying each row into this internal (DMA-capable)
    // stack buffer avoids a DMA bounce allocation per row inside the SPI driver.
    uint8_t row[PANEL_ROW_BYTES];

    sendCmd(cmd);

    gpio_set_level(gn(EPD_CS), 0);
    gpio_set_level(gn(EPD_DC), 1);
    for (int y = static_cast<int>(PANEL_HEIGHT) - 1; y >= 0; --y) {
        memcpy(row, fb + static_cast<uint32_t>(y) * PANEL_ROW_BYTES, PANEL_ROW_BYTES);
        spiTransmit(row, PANEL_ROW_BYTES);
    }
    for (uint16_t y = 0; y < CONTROLLER_PADDING_ROWS; ++y) {
        spiTransmit(white, PANEL_ROW_BYTES);
    }
    gpio_set_level(gn(EPD_CS), 1);
}

bool Uc8279cDisplay::refresh(bool turnOff) {
    sendCmd(CMD_DISPLAY_REFRESH);
    const int64_t t0 = esp_timer_get_time();
    bool started = false;
    while (esp_timer_get_time() - t0 < BUSY_START_CONFIRM_US) {
        if (gpio_get_level(gn(EPD_BUSY)) == 0) { started = true; break; }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (!started) {
        ESP_LOGW(TAG_UC, "refresh not started (BUSY stayed high), 命令可能被吞");
        return false;
    }
    waitBusy("refresh");
    if (turnOff) {
        sendCmd(CMD_POWER_OFF);
        waitBusy("power-off");
        screenOn_ = false;
    }
    return true;
}

bool Uc8279cDisplay::refreshActive() {
    return g_refreshDepth.load(std::memory_order_relaxed) > 0;
}

void Uc8279cDisplay::display(RefreshMode mode, bool turnOff) {
    if (offscreen_) return;
    RefreshScope scope;
    ensureController();
    waitIfBusy("pre-wait");
    const bool off = turnOff || autoPowerOff_;
    bool full = (mode == RefreshMode::Full) || fullRequested_;
    if (!full && fastRefreshLimit_ != 0 && fastRefreshesSinceFull_ >= fastRefreshLimit_) {
        full = true;
    }
    fullRequested_ = false;
    if (full && hasGrayContent()) {
        prevFrame_ = nullptr;
        displayGrayFrame(off);
        return;
    }
    displayBw(full, off);
    prevFrame_ = nullptr;
}

void Uc8279cDisplay::displayBw(bool full, bool turnOff) {
    if (grayControllerMode_) {
        hardwareReset();
        initController(false);
        // initController(false) fills DTM1 (the "old" frame the Fast LUT diffs
        // against) with white, but the panel still shows the grey frame and the
        // driver keeps no B/W copy of it. A Fast refresh here would only drive
        // pixels that differ from white, leaving a ghost of the previous image
        // (e.g. the old highlight). The first B/W refresh after a grey frame
        // must therefore use the Full waveform.
        full = true;
    }
    ensurePowerOn();

    const bool fast = !full;
    if (fast) {
        if (fastRefreshesSinceFull_ < 0xff) ++fastRefreshesSinceFull_;
    } else {
        fastRefreshesSinceFull_ = 0;
    }

    const uint8_t interval = fast ? 0xd7 : 0x97;
    cmdData(CMD_VCOM_DATA_INTERVAL, &interval, 1);
    if (fast) loadFastLut(); else loadFullLut();

    const uint8_t* previous = fast ? prevFrame_ : nullptr;
    if (previous != nullptr) writeFrame(CMD_DTM1, previous);
    writeFrame(CMD_DTM2, fb_);
    if (!refresh(turnOff)) {
        ESP_LOGW(TAG_UC, "display: 刷新未启动，重置控制器重试");
        waitIfBusy("retry-wait");
        hardwareReset();
        initController(false);
        ensurePowerOn();
        cmdData(CMD_VCOM_DATA_INTERVAL, &interval, 1);
        if (fast) loadFastLut(); else loadFullLut();
        if (previous != nullptr) writeFrame(CMD_DTM1, previous);
        writeFrame(CMD_DTM2, fb_);
        if (!refresh(turnOff)) {
            ESP_LOGE(TAG_UC, "display: 重试仍失败，画面可能残留");
        }
    }
}

void Uc8279cDisplay::waitRefreshComplete() {
    if (offscreen_) return;
    waitIfBusy("refresh");
}

void Uc8279cDisplay::displayGray(bool turnOff) {
    if (offscreen_) return;
    RefreshScope scope;
    ensureController();
    prevFrame_ = nullptr;
    waitIfBusy("gray-pre-wait");
    fullRequested_ = false;
    const bool off = turnOff || autoPowerOff_;
    if (!planesActive_ || !fbGrayMsb_ || !fbGrayLsb_) {
        displayBw(true, off);
        return;
    }
    displayGrayFrame(off);
}

void Uc8279cDisplay::displayGrayFrame(bool turnOff) {
    hardwareReset();
    initController(true);

    writeFrame(CMD_DTM1, fbGrayMsb_);
    writeFrame(CMD_DTM2, fbGrayLsb_);
    loadGrayLut();

    if (!refresh(turnOff)) {
        ESP_LOGW(TAG_UC, "displayGray: 刷新未启动，重置控制器重试");
        waitIfBusy("retry-wait");
        hardwareReset();
        initController(true);
        writeFrame(CMD_DTM1, fbGrayMsb_);
        writeFrame(CMD_DTM2, fbGrayLsb_);
        loadGrayLut();
        if (!refresh(turnOff)) {
            ESP_LOGE(TAG_UC, "displayGray: 重试仍失败，画面可能残留");
        }
    }
    fastRefreshesSinceFull_ = 0;
}

void Uc8279cDisplay::deepSleep() {
    if (offscreen_) return;
    fastRefreshesSinceFull_ = 0;
    prevFrame_ = nullptr;
    if (controllerPending_) return;
    if (screenOn_) {
        sendCmd(CMD_POWER_OFF);
        waitBusy("power-off");
        screenOn_ = false;
    }
    const uint8_t check = 0xa5;
    cmdData(CMD_DEEP_SLEEP, &check, 1);
}

void Uc8279cDisplay::waitBusy(const char* tag) {
    (void)tag;
    const int64_t start = esp_timer_get_time();

    bool busy = (gpio_get_level(gn(EPD_BUSY)) == 0);
    if (!busy) {
        while (esp_timer_get_time() - start < BUSY_SETTLE_US) {
            if (gpio_get_level(gn(EPD_BUSY)) == 0) {
                busy = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    int idleMs = 0;
    while (esp_timer_get_time() - start < BUSY_TIMEOUT_US) {
        if (gpio_get_level(gn(EPD_BUSY)) == 0) {
            idleMs = 0;
        } else if (++idleMs >= BUSY_IDLE_CONFIRM_MS) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void Uc8279cDisplay::waitIfBusy(const char* tag) {
    if (gpio_get_level(gn(EPD_BUSY)) != 0) return;
    waitBusy(tag);
}

void Uc8279cDisplay::spiTransmit(const uint8_t* data, size_t len) {
    if (!spi_) {
        ESP_LOGE(TAG_UC, "spiTransmit: spi_ is NULL! len=%u", (unsigned)len);
        return;
    }
    spi_transaction_t t = {};
    t.length = static_cast<size_t>(len) * 8;
    t.tx_buffer = data;
    const esp_err_t err = spi_device_polling_transmit(spi_, &t);
    if (err != ESP_OK) {
        static uint32_t failures = 0;
        if (failures++ % 64 == 0) {
            ESP_LOGE(TAG_UC, "spi transmit failed: %s (len=%u, %u failures)", esp_err_to_name(err),
                     (unsigned)len, (unsigned)failures);
        }
    }
}

void Uc8279cDisplay::sendCmd(uint8_t cmd) {
    gpio_set_level(gn(EPD_CS), 0);
    gpio_set_level(gn(EPD_DC), 0);
    spiTransmit(&cmd, 1);
    gpio_set_level(gn(EPD_CS), 1);
}

void Uc8279cDisplay::sendData(uint8_t data) {
    gpio_set_level(gn(EPD_CS), 0);
    gpio_set_level(gn(EPD_DC), 1);
    spiTransmit(&data, 1);
    gpio_set_level(gn(EPD_CS), 1);
}

void Uc8279cDisplay::sendData(const uint8_t* data, size_t len) {
    gpio_set_level(gn(EPD_CS), 0);
    gpio_set_level(gn(EPD_DC), 1);
    spiTransmit(data, len);
    gpio_set_level(gn(EPD_CS), 1);
}

void Uc8279cDisplay::cmdData(uint8_t cmd, const uint8_t* data, size_t len) {
    gpio_set_level(gn(EPD_CS), 0);
    gpio_set_level(gn(EPD_DC), 0);
    spiTransmit(&cmd, 1);
    if (len > 0 && data != nullptr) {
        gpio_set_level(gn(EPD_DC), 1);
        spiTransmit(data, len);
    }
    gpio_set_level(gn(EPD_CS), 1);
}

void Uc8279cDisplay::clear() {
    if (fb_) {
        memset(fb_, 0xff, FRAMEBUFFER_SIZE);
    }
    planesActive_ = false;
}

void Uc8279cDisplay::drawPixel(int16_t x, int16_t y, uint8_t color) {
    if (!fb_) return;
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;

    const uint16_t nativeX = static_cast<uint16_t>(y);
    const uint16_t nativeY = static_cast<uint16_t>(PANEL_HEIGHT - 1 - x);

    const uint32_t byteIdx = static_cast<uint32_t>(nativeY) * PANEL_ROW_BYTES + (nativeX >> 3);
    const uint8_t bit = 7 - (nativeX & 0x7);

    if (color) {
        fb_[byteIdx] |= (1 << bit);
    } else {
        fb_[byteIdx] &= ~(1 << bit);
    }

    if (planesActive_) {
        setGrayPlanes(byteIdx, bit, color ? 3 : 0);
    }
}

uint8_t Uc8279cDisplay::getPixel(int16_t x, int16_t y) const {
    if (!fb_) return 1;
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return 1;

    const uint16_t nativeX = static_cast<uint16_t>(y);
    const uint16_t nativeY = static_cast<uint16_t>(PANEL_HEIGHT - 1 - x);

    const uint32_t byteIdx = static_cast<uint32_t>(nativeY) * PANEL_ROW_BYTES + (nativeX >> 3);
    const uint8_t bit = 7 - (nativeX & 0x7);

    return (fb_[byteIdx] >> bit) & 1;
}

void Uc8279cDisplay::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color) {
    if (!fb_) return;
    if (w <= 0 || h <= 0) return;

    int16_t x0 = (x < 0) ? 0 : x;
    int16_t y0 = (y < 0) ? 0 : y;
    int16_t x1 = (x + w > WIDTH) ? WIDTH : (x + w);
    int16_t y1 = (y + h > HEIGHT) ? HEIGHT : (y + h);

    for (int16_t py = y0; py < y1; ++py) {
        for (int16_t px = x0; px < x1; ++px) {
            drawPixel(px, py, color);
        }
    }
}

void Uc8279cDisplay::ensureGrayBuffers() {
    if (planesActive_) return;
    if (!fbGrayMsb_) {
        fbGrayMsb_ = static_cast<uint8_t*>(heap_caps_malloc(FRAMEBUFFER_SIZE, MALLOC_CAP_SPIRAM));
    }
    if (!fbGrayLsb_) {
        fbGrayLsb_ = static_cast<uint8_t*>(heap_caps_malloc(FRAMEBUFFER_SIZE, MALLOC_CAP_SPIRAM));
    }
    if (!fb_ || !fbGrayMsb_ || !fbGrayLsb_) return;
    memcpy(fbGrayMsb_, fb_, FRAMEBUFFER_SIZE);
    memcpy(fbGrayLsb_, fb_, FRAMEBUFFER_SIZE);
    planesActive_ = true;
}

void Uc8279cDisplay::setGrayPlanes(uint32_t byteIdx, uint8_t bit, uint8_t level) {
    const uint8_t mask = static_cast<uint8_t>(1u << bit);
    if (level & 0x2) {
        fbGrayMsb_[byteIdx] |= mask;
    } else {
        fbGrayMsb_[byteIdx] &= static_cast<uint8_t>(~mask);
    }
    if (level & 0x1) {
        fbGrayLsb_[byteIdx] |= mask;
    } else {
        fbGrayLsb_[byteIdx] &= static_cast<uint8_t>(~mask);
    }
}

void Uc8279cDisplay::drawPixelGray(int16_t x, int16_t y, uint8_t gray) {
    if (!fb_) return;
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
    if (gray > 15) gray = 15;

    const uint16_t nativeX = static_cast<uint16_t>(y);
    const uint16_t nativeY = static_cast<uint16_t>(PANEL_HEIGHT - 1 - x);
    const uint32_t byteIdx = static_cast<uint32_t>(nativeY) * PANEL_ROW_BYTES + (nativeX >> 3);
    const uint8_t bit = 7 - (nativeX & 0x7);

    if (grayToBw(gray)) {
        fb_[byteIdx] |= static_cast<uint8_t>(1u << bit);
    } else {
        fb_[byteIdx] &= static_cast<uint8_t>(~(1u << bit));
    }

    const uint8_t level = gray >> 2;
    if (!planesActive_) {
        if (level == 0 || level == 3) return;
        ensureGrayBuffers();
        if (!planesActive_) return;
    }
    setGrayPlanes(byteIdx, bit, level);
}

uint8_t Uc8279cDisplay::grayLevel(int16_t x, int16_t y) const {
    if (!fb_) return 3;
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return 3;
    const uint16_t nativeX = static_cast<uint16_t>(y);
    const uint16_t nativeY = static_cast<uint16_t>(PANEL_HEIGHT - 1 - x);
    const uint32_t byteIdx = static_cast<uint32_t>(nativeY) * PANEL_ROW_BYTES + (nativeX >> 3);
    const uint8_t bit = 7 - (nativeX & 0x7);
    if (planesActive_ && fbGrayMsb_ && fbGrayLsb_) {
        return static_cast<uint8_t>((((fbGrayMsb_[byteIdx] >> bit) & 1) << 1) |
                                    ((fbGrayLsb_[byteIdx] >> bit) & 1));
    }
    return ((fb_[byteIdx] >> bit) & 1) ? 3 : 0;
}

void Uc8279cDisplay::clearGray() {
    planesActive_ = false;
}

bool Uc8279cDisplay::hasGrayContent() const {
    if (!planesActive_ || !fbGrayMsb_ || !fbGrayLsb_) return false;
    for (uint32_t i = 0; i < FRAMEBUFFER_SIZE; ++i) {
        if ((fbGrayMsb_[i] ^ fbGrayLsb_[i]) != 0) return true;
    }
    return false;
}

}  // namespace eego
