// SPDX-License-Identifier: GPL-3.0-or-later
// 移植自 eego-a4-weread（GPL-3.0）同名文件；原始硬件事实来自 eego-a4-template-firmware（MIT）。
#pragma once

// UC8279C 墨水屏显示驱动 — 纯 ESP-IDF 实现
// 面板控制器 768×600，固件 framebuffer 768×552 + 48 行白填充
// 逻辑 portrait 坐标 (x∈[0,551], y∈[0,767]) 映射到 native (nativeX=y, nativeY=551-x)
// BUSY active-low: 忙时 LOW，空闲 HIGH
// 0=黑, 1=白

#include "eego_pins.h"

#include <driver/spi_master.h>
#include <esp_err.h>
#include <stdint.h>

namespace eego {

// Fast: 快速 B/W 波形（灰度像素按阈值渲染为黑/白，不走灰阶波形）。
// Full: 完整波形；帧内含灰度时走原生四级灰阶波形。
enum class RefreshMode { Fast, Full };

class Uc8279cDisplay {
public:
    Uc8279cDisplay();
    ~Uc8279cDisplay();

    // 初始化 SPI + GPIO + 控制器。deferController=true（电池定时唤醒）：只分配
    // framebuffer 并初始化 SPI/GPIO，控制器上电/复位/初始化推迟到第一次
    // display()/displayGray()；从未刷新则 deepSleep() 为空操作，面板保持上一帧。
    esp_err_t begin(bool deferController = false);
    // 离屏实例（例如管理 API 的渲染预览）：只分配 PSRAM framebuffer，不初始化
    // SPI/GPIO/控制器；display()/displayGray()/deepSleep() 为空操作，end()/析构
    // 只释放缓冲区（不释放实屏占用的 SPI 总线）。不得在实屏实例上调用。
    esp_err_t beginOffscreen();
    bool offscreen() const { return offscreen_; }
    void end();               // 释放资源

    // framebuffer 操作（逻辑 portrait 坐标）
    void clear();             // 全白
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color); // color: 0=黑 1=白
    void drawPixel(int16_t x, int16_t y, uint8_t color);  // color: 0=黑 1=白
    uint8_t getPixel(int16_t x, int16_t y) const;

    // 四级灰阶 (gray 0-15: 0-3=黑, 4-7=深灰, 8-11=浅灰, 12-15=白)
    // fb_ 始终保存整帧的 B/W 版本（灰度按 grayToBw 阈值：<8 黑，>=8 白），
    // 供 Fast 刷新使用。首个非纯黑/白灰度像素时延迟分配 MSB/LSB 灰阶平面并从
    // fb_ 同步；此后 drawPixel/fillRect 也同步更新灰阶平面，直到 clear()/clearGray()。
    void drawPixelGray(int16_t x, int16_t y, uint8_t gray);
    void clearGray();  // 丢弃灰阶平面（帧内容保持 fb_ 的 B/W 版本）
    // 当前帧是否仍含灰度像素（扫描灰阶平面 MSB!=LSB；按帧准确，局部重绘覆盖
    // 掉所有灰度后自动变为 false）。
    bool hasGrayContent() const;
    // 像素的四级灰阶（0=黑, 1=深灰, 2=浅灰, 3=白；Full 刷新所显示的级别）。
    // 无灰阶平面时按 B/W 返回 0/3。
    uint8_t grayLevel(int16_t x, int16_t y) const;
    static uint8_t grayToBw(uint8_t gray) { return gray >= 8 ? 1 : 0; }

    // 刷新（阻塞直到波形完成）。
    // Fast：B/W 快速波形；连续 fastRefreshLimit 次 Fast 后自动升级为一次 Full。
    // Full（或 requestFullRefresh() 之后的下一次刷新）：帧含灰度 → 四级灰阶波形，
    // 否则 B/W 完整波形。turnOff 或 autoPowerOff 为真时刷新后关闭面板高压。
    void display(RefreshMode mode = RefreshMode::Fast, bool turnOff = false);
    void waitRefreshComplete();  // 仅在 BUSY 仍为忙时等待（阻塞 display 后立即返回）
    // 显式四级灰阶刷新：MSB→DTM1, LSB→DTM2，加载灰阶 LUT。无灰阶平面时回退为 B/W Full。
    void displayGray(bool turnOff = false);

    // 跨控制器复位的 Fast 刷新：nativeFrame（与 framebuffer() 同布局，768×552 1bpp，
    // 须保持有效直到下一次 display() 返回）是面板上实际显示的上一帧。下一次 B/W
    // Fast 刷新先把它写入 DTM1（旧帧）再写 DTM2，使深睡/复位后也能做 Fast 刷新。
    // 仅作用于下一次 display()；Full/灰阶刷新忽略它。
    void setPreviousFrame(const uint8_t* nativeFrame) { prevFrame_ = nativeFrame; }

    // 刷新策略
    void requestFullRefresh() { fullRequested_ = true; }  // 下一次 display() 走 Full
    void setFastRefreshLimit(uint8_t n) { fastRefreshLimit_ = n; }  // 0=不自动升级
    uint8_t fastRefreshLimit() const { return fastRefreshLimit_; }
    void setAutoPowerOff(bool on) { autoPowerOff_ = on; }  // 每次刷新后关闭面板高压
    bool autoPowerOff() const { return autoPowerOff_; }

    // 电源
    void deepSleep();

    // 几何
    static constexpr uint16_t WIDTH = LOGICAL_WIDTH;   // 552
    static constexpr uint16_t HEIGHT = LOGICAL_HEIGHT; // 768

    uint8_t* framebuffer() { return fb_; }
    const uint8_t* framebuffer() const { return fb_; }
    // 灰阶平面（帧含灰度时有效，否则 nullptr）：与 framebuffer() 一起决定帧内容哈希。
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
    // 发起刷新并等待完成；返回 false 表示刷新命令未被控制器接受（BUSY 未拉低）
    bool refresh(bool turnOff);
    void waitBusy(const char* tag);
    void waitIfBusy(const char* tag);  // BUSY 当前为忙才进入 waitBusy
    void displayBw(bool full, bool turnOff);
    void displayGrayFrame(bool turnOff);
    void setGrayPlanes(uint32_t byteIdx, uint8_t bit, uint8_t level);  // level 0-3
    void sendCmd(uint8_t cmd);
    void sendData(uint8_t data);
    void sendData(const uint8_t* data, size_t len);
    void cmdData(uint8_t cmd, const uint8_t* data, size_t len);
    void spiTransmit(const uint8_t* data, size_t len);
    void ensureGrayBuffers();  // 延迟分配 + 首次从 fb_ 同步灰阶平面

    spi_device_handle_t spi_ = nullptr;
    uint8_t* fb_ = nullptr;           // PSRAM framebuffer, native 方向 (768×552/8)
    uint8_t* fbGrayMsb_ = nullptr;    // PSRAM 灰阶 MSB 平面 (与 fb_ 同尺寸)
    uint8_t* fbGrayLsb_ = nullptr;    // PSRAM 灰阶 LSB 平面 (与 fb_ 同尺寸)
    bool planesActive_ = false;       // 灰阶平面已同步且随绘制更新
    bool offscreen_ = false;          // beginOffscreen() 实例：无 SPI/面板
    bool screenOn_ = false;
    bool grayControllerMode_ = false; // 控制器当前是否处于灰阶模式
    bool fullRequested_ = false;
    bool controllerPending_ = false;  // begin(true)：控制器尚未上电初始化
    const uint8_t* prevFrame_ = nullptr;
    bool autoPowerOff_ = true;
    uint8_t fastRefreshesSinceFull_ = 0;
    uint8_t fastRefreshLimit_ = DEFAULT_FAST_REFRESH_LIMIT;
    static constexpr uint8_t DEFAULT_FAST_REFRESH_LIMIT = 8;
};

}  // namespace eego
