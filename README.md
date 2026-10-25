# EEGO A4

面向 EEGO A4（ESP32-S3 + 7.8 英寸 UC8279C 墨水屏）的可配置告警/数据面板。

主产品是 `firmware/` 下的 ESP-IDF 固件：支持 HTTP 数据源、声明式解析、变量、
页面布局、告警、独立刷新，以及触控、WiFi/BLE、前光、存储和休眠。设备不依赖
Arduino，也不执行 Python/JavaScript 等任意脚本。

- [固件构建与使用](firmware/README.md)
- [面板预设与示例](firmware/presets/README.md)

## 快速开始

需要 Python 3、ESP-IDF v5.5.1 和支持数据的 USB 线。在仓库根目录执行：

```sh
python3 firmware/tools/validate_firmware.py
python3 firmware/tools/preset_tool.py validate-library
idf.py -C firmware -B .tmp/firmware-build build
```

刷写只走 USB，没有远程 OTA。写入前先按[备份与恢复](docs/RECOVERY.md)保存整片
Flash，并确认使用当前固件的分区表。

首次开机：

1. 连接设备 AP `EEGO-A4-Setup`，打开 `http://192.168.4.1/`。
2. 选择 WiFi，并设置至少 8 位管理密码；连接成功后设备保存设置并重启。
3. 打开 `http://<device-ip>/` 登录，在「数据面板」导入、校验并应用 bundle。
4. 可从 `firmware/presets/` 选择预设，用 `preset_tool.py export` 展开后导入。

数据源凭据通过设备「数据面板 → 凭据」写入，配置中只保存 `nvs:source/<key>`
引用，不要把真实秘密写入预设、JSON、URL 或日志。

## 旧 Arduino 模板

`src/`、`lib/` 和 `examples/` 是保留的硬件诊断与驱动开发基线，不是当前主产品；
其示例和实机结论不能当作新固件的验收结果。

```sh
python3 scripts/validate_project.py --quick  # 静态与 Markdown 链接检查
python3 scripts/validate_project.py          # 同时编译诊断固件和五个示例
```

常用参考资料：

- [硬件引脚与电气事实](docs/HARDWARE_REFERENCE.md)
- [驱动 API 与启动顺序](docs/DRIVER_GUIDE.md)
- [显示安全区](docs/SAFE_AREA.md)
- [备份、刷写与恢复](docs/RECOVERY.md)
- [可编译示例](examples/README.md)

## 目录

```text
firmware/    当前 ESP-IDF 固件、预设和主机工具
src/ lib/    旧 Arduino 诊断模板与驱动
examples/    旧模板示例
docs/        硬件、驱动和恢复参考
scripts/     仓库校验、打包和安全刷写工具
```

`.tmp/`、`.pio/`、`release/` 和 `captures/` 是生成目录，不应提交。

## 许可

旧模板采用 [MIT](LICENSE)，`firmware/` 采用
[GPL-3.0-or-later](firmware/LICENSE)。来源和第三方组件见
[NOTICE.md](NOTICE.md)、[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) 及
`firmware/` 下的对应声明。
