# Arduino 示例

这些示例属于旧硬件模板，用于验证单项驱动，不是当前 ESP-IDF 主固件。
每个环境都可独立编译：

| 环境 | 内容 |
|---|---|
| `eego_a4_quickstart` | 安全启动、屏幕、触控和按键 |
| `eego_a4_storage_rtc_battery` | SD、RTC、电池和充电状态 |
| `eego_a4_wifi_ble` | WiFi/BLE 扫描与资源清理 |
| `eego_a4_safe_sleep` | 带定时唤醒的安全深睡 |
| `eego_a4_cpfont` | 从 SD 加载并绘制 CPFONT 中文 |

```sh
pio run -e eego_a4_quickstart
python3 scripts/validate_project.py  # 编译诊断固件和全部示例
```

所有示例先调用 `eego::holdPower()`，再使用 `eego::beginStandardHardware()`；
普通 UI 调用 `canvas.useUiContentRect()`，遵守[显示安全区](../docs/SAFE_AREA.md)。
不要复制引脚常量或修改触控原始标定。

上传会覆盖 app 分区。第一次安装、分区未知或需要恢复时，不要使用快捷 upload，
先阅读[备份与恢复](../docs/RECOVERY.md)。
