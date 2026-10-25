# EEGO A4 硬件参考

本文记录当前 ESP-IDF 固件需要的板级事实。未列出的 GPIO 不代表已连接外设；
未知器件不要主动写寄存器。

## 基本配置与引脚

- ESP32-S3，16 MiB Flash，8 MiB Octal PSRAM；2.4 GHz WiFi + BLE，无 Classic。
- UC8279C 墨水屏、GSLX680 触控、PCF8563 RTC、独立 SPI microSD。
- 逻辑显示方向为 552×768 竖屏，原生 framebuffer 为 768×552。

| GPIO | 功能 | 参数 |
|---:|---|---|
| 1 / 2 | I²C SCL / SDA | 400 kHz；触控、RTC、可选前光共享 |
| 4 | 主电源保持 | 高有效，启动时最先拉高，睡眠中保持 |
| 5 / 7 | Up / Down | 低有效，输入上拉 |
| 6 | 墨水屏电源 | 高有效，只由显示/电源驱动管理 |
| 8 | Power | 高有效，输入下拉 |
| 10 | 电池 ADC | 实际 mV = ADC mV × 1.559 |
| 11 | 充电状态 | 有 LM3630A 时高有效，否则低有效 |
| 12 | 可选前光 enable | 高有效；未探测到芯片时保持低 |
| 13 / 14 | 墨水屏 RESET / D/C | RESET 高 20 ms、低 10 ms、高 100 ms |
| 21 / 42 / 45 / 41 | 墨水屏 CS / SCLK / MOSI / BUSY | SPI3_HOST，mode 0，20 MHz，BUSY 低有效 |
| 47 / 39 / 38 / 40 | SD CS / SCLK / MOSI / MISO | 独立 SPI2_HOST，20 MHz |

SD 与显示分别持有 SPI2/SPI3，不得合并或重新映射。I²C 使用 `I2C_NUM_0`，由
GSLX680、PCF8563 和 LM3630A 共享；不要在多个任务中无锁访问。

`board::beginStandardHardware()` 的顺序为 GPIO4 → 触控/按键 → 可选前光与充电
极性 → RTC → SD → 显示。SD 必须先于显示初始化，具体合同见
`firmware/components/board/include/board/board_boot.h`。

## 显示

- 控制器扫描为 768×600；主机 framebuffer 为 768×552、1 bpp、每行 96 字节，
  共 52,992 字节。发送时按 `y=551..0` 逆序并补 48 行白色，不在应用中扩展缓冲区。
- 竖屏逻辑坐标转换：`nativeX=logicalY`，`nativeY=551-logicalX`。
- bit 7 是字节最左像素，`0` 黑、`1` 白。四灰阶使用两个 1-bit plane，工作区从
  PSRAM 分配。
- Fast 连续最多 4 次，下一次自动提升为 Full；页面切换或残影明显时主动 Full。
- 正文与控件遵守[显示安全区](SAFE_AREA.md)。

## 触控与按键

GSLX680 地址为 `0x40`，启动需上传 36,696 字节控制器固件。当前表位于
`firmware/components/eego_hal/include/gslx680_firmware.h`，可用
`scripts/extract_eego_a4_gsl_firmware.py` 从合法持有的指定镜像复现。

驱动的官方全范围标定为：

```text
portrait_x = min(raw_y, 680) * 551 / 680
portrait_y = (920 - min(raw_x, 920)) * 767 / 920
native_x   = portrait_y
native_y   = 551 - portrait_x
```

应用只做 panel-native 到 logical 的旋转，不再次镜像 raw X/Y。屏下键是特殊记录
`rawXWord=0x03a0, rawYWord=0x1020`，必须在普通坐标 12-bit mask 前识别，不能当作
角落触点；短按、长按和自定义动作由输入层处理。

## 存储、RTC、前光与电源

- RTC：PCF8563 地址 `0x51`，时间从寄存器 `0x02` 开始，7 字节 BCD。秒寄存器
  bit 7 为 VL；置位时设备可应答但时间无效，必须设时后再使用。
- 前光：可选 LM3630A，地址 `0x36`、enable GPIO12。无 ACK 时按未装配处理，后续
  亮度操作应为安全 no-op，不要假定所有设备都有前光。
- 电池：GPIO10 经分压采样，百分比只是电压曲线估值，不是精确电量计。GPIO11
  是充电状态输入而非充电控制；其有效极性随 LM3630A 是否存在而变化。
- 正常运行绝不能拉低 GPIO4。深睡前必须保持 GPIO4 并安排唤醒条件；唤醒后解除
  GPIO hold 并重新拉高电源保持。
- IMU、温湿度、音频、麦克风、LED 和蜂鸣器没有已确认的板载证据，不应虚构驱动。

## 分区

当前布局以 [`firmware/partitions.csv`](../firmware/partitions.csv) 为准：16 MiB
Flash、A/B app、NVS、SPIFFS 和 coredump。预留 A/B 分区不代表开放远程 OTA。

刷写使用同一个 ESP-IDF 构建目录及 `idf.py flash`，不要混用其他项目的镜像或手工
猜测偏移。备份与恢复流程见[RECOVERY.md](RECOVERY.md)。
