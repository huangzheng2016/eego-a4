# EEGO A4

面向 EEGO A4（ESP32-S3 + 7.8 英寸 UC8279C 墨水屏）的可配置告警/数据面板。
仓库只保留纯 ESP-IDF 固件和主机工具，支持 HTTP 数据源、声明式解析、变量、
页面布局、告警、独立刷新，以及触控、WiFi/BLE、前光、存储和休眠。设备不执行
Python/JavaScript 等任意脚本。

- [固件构建与使用](firmware/README.md)
- [面板预设与示例](firmware/presets/README.md)

## 构建

需要 ESP-IDF v5.5.1。在仓库根目录执行：

```sh
python3 firmware/tools/validate_firmware.py
python3 firmware/tools/preset_tool.py validate-library
idf.py -C firmware -B .tmp/firmware-build build
```

刷写只走 USB，没有远程 OTA。写入前先按[备份与恢复](docs/RECOVERY.md)保存整片
Flash，并使用当前构建生成的 bootloader、分区表和应用镜像。

首次开机：

1. 连接设备 AP `EEGO-A4-Setup`，打开 `http://192.168.4.1/`。
2. 选择 WiFi，并设置至少 8 位管理密码；连接成功后设备保存设置并重启。
3. 打开 `http://<device-ip>/` 登录，在「数据面板」导入、校验并应用 bundle。
4. 可从 `firmware/presets/` 选择预设，用 `preset_tool.py export` 展开后导入。

数据源凭据通过设备「数据面板 → 凭据」写入，配置中只保存 `nvs:source/<key>`
引用，不要把真实秘密写入预设、JSON、URL 或日志。

## 参考资料

- [硬件引脚与电气事实](docs/HARDWARE_REFERENCE.md)
- [显示安全区](docs/SAFE_AREA.md)
- [备份、刷写与恢复](docs/RECOVERY.md)

## 目录

```text
firmware/    ESP-IDF 固件、预设和主机工具
docs/        硬件、安全区和恢复参考
scripts/     触控固件表提取工具
```

`.tmp/` 和其他生成目录不应提交。

## 许可

`firmware/` 采用 [GPL-3.0-or-later](firmware/LICENSE)。根目录工具及历史硬件研究的
许可与来源见 [LICENSE](LICENSE)、[NOTICE.md](NOTICE.md)；第三方组件见
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) 和固件目录下的对应声明。
