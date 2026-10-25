# 旧 Arduino 模板驱动指南

本文只描述 `src/`、`lib/` 和 `examples/` 的旧模板，不是当前 ESP-IDF 固件架构。
板级引脚和电气事实见[硬件参考](HARDWARE_REFERENCE.md)。

## 启动与板级配置

应用应使用统一启动器：

```cpp
void setup() {
  eego::holdPower();
  delay(250);
  eego::beginUsbSerial();

  const eego::BeginStatus hw =
      eego::beginStandardHardware(display, input, frontlight, rtc, sd);
  if (!hw.displayReady) {
    return;
  }
}
```

初始化顺序为 GPIO4 → 触控/按键 → 可选前光与充电极性 → RTC → SD → 显示。
SD 必须先于显示初始化，两者分别持有独立 SPI 总线；I²C 由触控、RTC 和前光共享。
不要在应用层散落引脚号，板级参数集中在 `BoardConfig::EEGO_A4` 和
`lib/EegoA4Support/include/EegoA4Hardware.h`。编译定义由 `platformio.ini` 提供：
`FREEINK_DEVICE_EEGO_A4=1`。

## 显示与坐标

```cpp
display.clearScreen(0xff);
display.displayBuffer(EInkDisplay::FULL_REFRESH, false);

canvas.useUiContentRect();
canvas.setCursor(eego::UI_CONTENT_X + 4, eego::UI_CONTENT_Y + 4);
canvas.print("TITLE");
```

framebuffer 为 768×552、1 bpp；`PortraitCanvas` 提供 552×768 竖屏 Adafruit GFX
坐标，转换为 `nativeX=logicalY`、`nativeY=551-logicalX`。普通 UI 必须调用
`useUiContentRect()`；只有全屏背景或硬件测试可短暂 `useFullPanel()`，之后立即恢复。
详见[显示安全区](SAFE_AREA.md)。

刷新可用 `FAST_REFRESH`、`HALF_REFRESH`、`FULL_REFRESH`。HALF/FULL 都使用完整
清理波形；连续 Fast 最多 4 次，下一次自动提升为 Full。四灰阶使用
`copyGrayscaleBuffers()`、`displayGrayBuffer()`，两个 plane 及驱动工作区应从 PSRAM
分配，不要挤占内部 RAM。

## 输入

```cpp
input.update();
if (input.wasPressed(InputManager::BTN_POWER)) { /* Power */ }

float nx, ny;
if (input.wasTouchTap(nx, ny)) {
  int16_t x, y;
  eego::normalizedTouchToLogical(nx, ny, x, y);
}
```

触控返回 panel-native 归一化坐标，驱动已经完成 raw 标定和 native-Y 反射；应用只做
逻辑旋转，不再次镜像。屏下键使用 `wasHomeKeyShortPressed()` /
`wasHomeKeyLongPressed()`，不要自行解析 sentinel。

刷新期间不能丢输入时，使用 `input.beginAsync()` 和 `popPress()` /
`popTouchTap()` / `popSwipe()`。启用后台采样后，主线程不得再调用 `update()`。

## 外设

```cpp
SDCardManager& sd = SDCardManager::getInstance();
if (sd.ready()) {
  sd.writeFile("/test.txt", "hello");
  String text = sd.readFile("/test.txt");
  sd.remove("/test.txt");
}

Rtc rtc;
Rtc::DateTime now;
bool valid = rtc.begin() && rtc.now(now);

BatteryMonitor battery;
BatteryMonitor::Status status = battery.readStatus();

if (frontlight.present()) {
  frontlight.setBrightness(10);
  frontlight.off();
}
```

- RTC 的 VL 标志置位时 `now()` 返回 false，先设置有效时间。
- 电池百分比是估值；充电极性由统一启动器随可选前光探测结果配置。
- 前光只在 `present()` 后操作；未装配时不要强制拉 GPIO12。
- I²C 使用 `Wire.begin(2, 1, 400000)`，扫描只做 ACK，不向未知地址写寄存器。

WiFi/BLE 测试结束后释放资源：

```cpp
WiFi.scanDelete();
WiFi.disconnect(true, false);
WiFi.mode(WIFI_OFF);

NimBLEScan* scan = NimBLEDevice::getScan();
scan->clearResults();
NimBLEDevice::deinit(true);
```

`disconnect(true, false)` 不擦除已保存的 AP。NimBLE-Arduino 2.x 的扫描时长单位是
毫秒；完整用法见 `examples/03_wifi_ble_scan/main.cpp`。

## 深睡与字体

```cpp
eego::enterTimedDeepSleep(display, frontlight, 10);
```

helper 会等待刷新、关闭前光、保持 GPIO4，并先设置 3–60 秒定时唤醒再深睡。
不要测试无唤醒条件的深睡；量产功耗仍需实测各电源轨。

CPFONT 最小读取器用法：

```cpp
CpFontRenderer font(sd, canvas);
if (font.load("/fonts/MiSansA4/MiSansA4_14.cpfont")) {
  uint16_t missing = 0, ioErrors = 0;
  font.drawText(32, 120, "English 中文", &missing, &ioErrors);
}
```

该读取器只覆盖 regular、LTR、单行、黑白路径，不包含完整排版、fallback 或灰阶。

## 诊断与排错

旧诊断固件使用 115200 baud：

```sh
pio device monitor -b 115200 -p <PORT>
```

常用命令为 `help`、`status`、`run all`、`run touch`、`run buttons`、`report json`、
`ui state`、`screenshot`。自动测试不能替代触控、实体按键、显示残影和功耗的人工
检查；`screenshot` 只回读 MCU framebuffer，不能证明实体面板已正确刷新。

常见优先检查：掉 USB 即关机先看 GPIO4；framebuffer 正确但屏幕不更新先看 EPD/SD
总线；触摸左右反了先移除应用层重复镜像；RTC 读取失败先看 VL；深睡不醒先看
GPIO4 hold 和 timer wake。刷写异常按[恢复流程](RECOVERY.md)处理。
