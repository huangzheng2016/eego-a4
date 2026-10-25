// SPDX-License-Identifier: GPL-3.0-or-later
// board_boot.cpp — EEGO A4 板级启动事实实现
//
// 顺序即合同，调整前请先读 board_boot.h 头部与 docs/HARDWARE_REFERENCE.md：
// "SD 必须先于显示初始化"（两条独立 SPI 总线各自持有所有权）。
#include "board/board_boot.h"

#include "eego_hal.h"

namespace eego {
namespace board {

BootStatus beginStandardHardware(Uc8279cDisplay& display, Gslx680Touch& touch) {
    BootStatus st;

    // 1. GPIO4 电源锁存 — 系统启动后首项硬件操作
    PowerManager::instance().holdPower();
    st.powerLatched = true;

    // 2. 触控（GSLX680 固件上传，共享 I2C_NUM_0）+ 三键轮询任务
    st.touchReady = (touch.begin(I2C_NUM_0) == ESP_OK);
    st.buttonsReady = (ButtonManager::instance().begin() == ESP_OK);

    // 3. 前光安全探测（LM3630A 缺席时 GPIO12 已回低电平）
    st.frontlightPresent = FrontlightManager::instance().present();

    // 4. 充电状态极性随前光在位与否：
    //    LM3630A 在位 → GPIO11 active-high（下拉），缺席 → active-low（上拉）。
    //    PowerManager 首次查询时读取前光缓存并配置 GPIO11。
    PowerManager::instance().isCharging();

    // 5. PCF8563 RTC（共享 I2C_NUM_0）
    Pcf8563Rtc rtc(I2C_NUM_0);
    st.rtcReady = (rtc.begin() == ESP_OK);

    // 6. SD 卡挂载（独立 SPI2_HOST）— 必须先于显示初始化
    st.sdReady = (SdCard::instance().mount() == ESP_OK);

    // 7. UC8279C 显示（独立 SPI3_HOST，PSRAM framebuffer）— 最后初始化。
    //    先全白 Full 刷一帧：清残影并尽早证明显示链路健康。
    st.displayReady = (display.begin() == ESP_OK);
    if (st.displayReady) {
        display.clear();
        display.display(RefreshMode::Full);
        display.waitRefreshComplete();
    }

    return st;
}

}  // namespace board
}  // namespace eego
