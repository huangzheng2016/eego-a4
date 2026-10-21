// board_boot.h — EEGO A4 板级启动事实（启动顺序合同）
//
// 本组件把 EEGO A4 的实机启动合同固化为一处可审计的实现，顺序继承自
// eego-a4-template-firmware（MIT）的 beginStandardHardware 与
// eego-a4-weread（GPL-3.0）的 WereadApp::initHardware：
//
//   1. GPIO4 电源锁存（首项操作，保持整机供电）
//   2. GSLX680 触控（共享 I2C_NUM_0 固件上传）+ 三键轮询任务
//   3. LM3630A 前光安全探测（缺席时 GPIO12 回低电平）
//   4. 充电状态极性随前光在位与否配置（GPIO11 上下拉）
//   5. PCF8563 RTC（共享 I2C_NUM_0）
//   6. SD 卡挂载（独立 SPI2_HOST）—— 必须先于显示初始化
//   7. UC8279C 显示 + PSRAM framebuffer（独立 SPI3_HOST，最后初始化）
//
// 总线所有权：EPD-SPI(SPI3) 归显示驱动，SD-SPI(SPI2) 归 SD 驱动，
// 应用不得直接操作；I2C_NUM_0 由 GSLX680 / PCF8563 / LM3630A 共享，
// legacy I2C 驱动的 i2c_master_*_device API 自带端口级锁（共享 I2C 锁），
// 各驱动 begin 均容忍总线已安装（ESP_ERR_INVALID_STATE）。
#pragma once

#include <esp_err.h>

namespace eego {

class Uc8279cDisplay;
class Gslx680Touch;

namespace board {

struct BootStatus {
    bool powerLatched = false;     // GPIO4 锁存已拉起
    bool touchReady = false;       // GSLX680 固件上传并校验通过
    bool buttonsReady = false;     // 三键轮询任务已启动
    bool frontlightPresent = false;// LM3630A 在位
    bool rtcReady = false;         // PCF8563 应答正常
    bool sdReady = false;          // SD 卡挂载成功
    bool displayReady = false;     // UC8279C 初始化成功
};

// 按上述合同顺序初始化全部板级硬件并返回各项状态。
// display / touch 由调用方持有（通常 app_main 的静态对象），本函数不接管所有权。
BootStatus beginStandardHardware(Uc8279cDisplay& display, Gslx680Touch& touch);

}  // namespace board
}  // namespace eego
