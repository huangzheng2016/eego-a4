# EEGO A4

EEGO A4（ESP32-S3 + 7.8 英寸墨水屏）的可配置数据/告警面板。固件基于 ESP-IDF，
管理界面是托管在 GitHub Pages 的网页，可经 WiFi 或蓝牙连接设备。

## 配置设备

**蓝牙（推荐，设备在身边时）**：用桌面版 Chrome/Edge 或 Android Chrome 打开
<https://huangzheng2016.github.io/eego-a4/>，点「通过蓝牙连接」，在系统弹窗输入
设备屏幕上的 6 位配对码。首次使用可直接在网页里设置管理密码和 WiFi。连接后会
用电脑时间校准设备时钟。用电池时仅在按键唤醒后可连接；可在设备「设置 → 蓝牙管理」
关闭。iOS 和 Firefox 不支持 Web Bluetooth。

**WiFi**：

1. 首次开机连接设备热点 `EEGO-A4-Setup`，打开 `http://192.168.4.1/`，选择 WiFi
   并设置管理密码（至少 8 位）。
2. 设备重启后屏幕显示管理地址，在同一网络中打开 `http://<设备 IP>/` 登录。
3. WiFi 连不上时设备会重新开启该热点，打开 `http://192.168.4.1/wifi-diag` 可查看
   原因并修改网络。

在「仪表盘」中选择模板或导入 JSON，校验后应用到设备。数据源密钥在「凭据」中写入，
配置里只保存引用，不会被读出。

## 设备操作

| 输入 | 动作 |
|---|---|
| ↑ / ↓ 短按 | 上一屏 / 下一屏；列表中为上一项 / 下一项 |
| ↑ / ↓ 长按 | 移动焦点 |
| 电源键 短按 | 有焦点时选择，否则休眠（外接电源时再按一次确认） |
| 电源键 长按 | 关机（5 秒内再按一次确认） |
| 前置键 短按 / 长按 / 双击 | 返回 / 设置 / 设备状态 |
| ↑ + ↓ 同按 | 保持唤醒 |

## 构建与刷写

需要 ESP-IDF v5.5.1。在仓库根目录执行：

```sh
python3 firmware/tools/validate_firmware.py
idf.py -C firmware -B .tmp/firmware-build build
```

首次刷写前先备份整片 Flash（16 MiB），备份含设备数据，不要公开：

```sh
python3 -m esptool --chip esp32s3 --port <PORT> read_flash 0x0 0x1000000 eego-a4-backup.bin
idf.py -C firmware -B .tmp/firmware-build -p <PORT> flash monitor
```

恢复：`python3 -m esptool --chip esp32s3 --port <PORT> write_flash 0x0 eego-a4-backup.bin`。
只支持 USB 刷写，没有 OTA。

可选：复制 `firmware/main/local_secrets.example.h` 为 `local_secrets.h`，在编译期写入
默认 WiFi 和管理密码。这样构建出的固件含明文密码，不要分享。

### 调试

- **网页**：浏览器开发者工具的控制台会打印每个失败的请求和蓝牙连接过程，前缀为
  `[eego api]` / `[eego ble]`。「系统 → 日志 → 设备调试日志」可查看设备最近的输出。
- **崩溃**：固件崩溃后会把转储写进 Flash。重启后「系统」页顶部显示崩溃的任务和回溯
  地址，可复制。用同一次构建解码：
  `xtensa-esp32s3-elf-addr2line -pfiaC -e .tmp/firmware-build/eego-a4-alert-panel.elf <地址...>`，
  或 `idf.py -C firmware -B .tmp/firmware-build -p <PORT> coredump-info` 读取完整转储。
- **串口**：USB 连接电脑后运行 `idf.py -C firmware -B .tmp/firmware-build -p <PORT> monitor`，
  可实时看到全部日志，崩溃时回溯会自动解码。按 `Ctrl+]` 退出。

主机测试：

```sh
cmake -S firmware/test/host -B .tmp/host-tests && cmake --build .tmp/host-tests -j
ctest --test-dir .tmp/host-tests --output-on-failure
```

## 网页端开发

```sh
cd web && npm install
npm run dev:mock                          # 无硬件，密码 admin
VITE_DEVICE=http://192.168.1.50 npm run dev   # 代理到真实设备
npm run lint && npm test && npm run build
```

推送到 `main` 后由 GitHub Actions 发布到 Pages，设备加载页从那里加载应用。

## 预设

`firmware/presets/` 含面板/布局预设和完整示例，同一套也内置在固件里。

```sh
python3 firmware/tools/preset_tool.py list
python3 firmware/tools/preset_tool.py export chart-kpi --mock-host <电脑局域网IP>:8978 --output .tmp/chart-kpi.json
python3 firmware/tools/mock_dashboard_server.py --host 0.0.0.0 --port 8978
```

修改预设后运行 `preset_tool.py embed` 重新生成内置模板。

## 许可

GPL-3.0-or-later，见 [LICENSE](LICENSE)。本项目是独立社区项目，与 EEGO、Xteink、
CrossLink 无关。

- 固件板级代码移植自 [eego-a4-weread](https://github.com/MokuMMk/eego-a4-weread)（GPL-3.0），
  部分硬件资料来自 eego-a4-template-firmware（MIT，© 2026 FreeInk）。
- `gslx680_firmware.h`、`uc8279c_luts.h` 为从用户自有固件镜像提取的互操作数据，
  可用 `scripts/extract_eego_a4_gsl_firmware.py` 复现。
- 第三方：LodePNG（zlib，许可见源码头部）；数字字体位图取自 Noto Sans CJK SC
  （SIL OFL 1.1，© Adobe）；ESP-IDF（Apache-2.0）。
