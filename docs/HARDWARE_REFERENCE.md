# EEGO A4 硬件参考

本文记录驱动需要的板级事实，适用于旧 Arduino 模板及与其共享硬件实现的固件。
未列出的 GPIO 不代表已连接外设；未知器件不要主动写寄存器。

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
| 21 / 42 / 45 / 41 | 墨水屏 CS / SCLK / MOSI / BUSY | SPI mode 0，20 MHz，BUSY 低有效 |
| 47 / 39 / 38 / 40 | SD CS / SCLK / MOSI / MISO | 独立 HSPI，20 MHz |

SD 与显示必须使用各自长期存活的 SPI 实例。不要在 SD 已挂载后用全局 `SPI`
重新映射显示，也不要在多个任务中无锁共享 `Wire`。

## 显示

- 控制器扫描为 768×600；主机 framebuffer 为 768×552、1 bpp、每行 96 字节，
  共 52,992 字节。发送时按 `y=551..0` 逆序并补 48 行白色，不在应用中扩展缓冲区。
- 竖屏逻辑坐标转换：`nativeX=logicalY`，`nativeY=551-logicalX`。
- bit 7 是字节最左像素，`0` 黑、`1` 白。四灰阶使用两个 1-bit plane，工作区从
  PSRAM 分配。
- Fast 连续最多 4 次，下一次自动提升为 Full；页面切换或残影明显时主动 Full。
- 正文与控件遵守[显示安全区](SAFE_AREA.md)。

## 触控与按键

GSLX680 地址为 `0x40`，启动需上传 36,696 字节控制器固件；原始表和生成工具见
`lib/InputManager/` 与 `scripts/extract_eego_a4_gsl_firmware.py`。

驱动的官方全范围标定为：

```text
portrait_x = min(raw_y, 680) * 551 / 680
portrait_y = (920 - min(raw_x, 920)) * 767 / 920
native_x   = portrait_y
native_y   = 551 - portrait_x
```

应用只做 panel-native 到 logical 的旋转，不再次镜像 raw X/Y。屏下键是特殊记录
`rawXWord=0x03a0, rawYWord=0x1020`，必须在普通坐标 12-bit mask 前识别，不能当作
角落触点。短按释放不足 700 ms，持续至少 700 ms 产生一次长按。

## 存储、RTC、前光与电源

- SD：独立 SCLK39/MISO40/MOSI38/CS47 总线；不要与显示 SPI 合并。
- RTC：PCF8563 地址 `0x51`，时间从寄存器 `0x02` 开始，7 字节 BCD。秒寄存器
  bit 7 为 VL；置位时设备可应答但时间无效，必须设时后再使用。
- 前光：可选 LM3630A，地址 `0x36`、enable GPIO12。无 ACK 时按未装配处理，后续
  亮度操作应为安全 no-op，不要假定所有设备都有前光。
- 电池：GPIO10 经分压采样，百分比只是电压曲线估值，不是精确电量计。GPIO11
  是充电状态输入而非充电控制；其有效极性随 LM3630A 是否存在而变化。
- 正常运行绝不能拉低 GPIO4。开发用深睡必须先保持 GPIO4，再设置定时唤醒；
  唤醒后解除 GPIO hold 并重新拉高电源保持。
- IMU、温湿度、音频、麦克风、LED 和蜂鸣器没有已确认的板载证据，不应虚构驱动。

## 分区

设备为 16 MiB Flash。当前 ESP-IDF 固件和旧模板使用不同布局，分别以
[`firmware/partitions.csv`](../firmware/partitions.csv) 和
[`partitions.csv`](../partitions.csv) 为准。

两者 app0 都从 `0x10000` 开始，但 app 大小和其他分区不同，不能因此混刷镜像。
首次改变分区布局需写完整镜像；app-only 更新只在已确认分区表匹配时使用。
备份与恢复流程见[RECOVERY.md](RECOVERY.md)。
