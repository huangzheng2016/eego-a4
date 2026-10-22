# EEGO A4 可配置告警/数据面板

[English](README.en.md) · [完整固件指南](firmware/README.md) ·
[English integrated guide](firmware/README.md#english-integrated-guide) ·
[文档导航](docs/README.md)

本仓库的主产品是 `firmware/` 下的 **ESP-IDF 可配置告警/数据面板固件**，面向
EEGO A4（ESP32-S3 + 7.8" UC8279C 墨水屏）。它把 HTTP 数据源、声明式解析、
变量、panel/layout、告警样式和独立刷新，与触控/自定义输入、WiFi/BLE、前光、
存储、诊断、休眠和恢复整合为同一固件；不依赖 Arduino，也不执行任意脚本。

根目录旧 Arduino 诊断模板仍保留为**硬件参考和驱动开发基线**，不是当前主产品。
旧模板、示例及其真机验证结论不得套用到新 ESP-IDF 固件。

## 快速开始

需要 Python 3、ESP-IDF v5.5.1 和支持数据的 USB 线。先运行主机校验并完整构建：

```sh
python3 firmware/tools/validate_firmware.py
python3 firmware/tools/preset_tool.py validate-library
idf.py -C firmware -B .tmp/firmware-build build
idf.py -C firmware -B .tmp/firmware-build size
idf.py -C firmware -B .tmp/firmware-build size-components
```

刷写只走 USB；先备份整片、核对分区和镜像哈希，具体安全流程见
[恢复参考](docs/RECOVERY.md)和[固件指南](firmware/README.md)。首版没有远程 OTA。

首次开机：

1. 无管理密码时，连接设备 AP `EEGO-A4-Setup`，打开 `http://192.168.4.1/`。
2. 选择 WiFi，并设置至少 8 位管理密码；STA 试连成功后设备保存并重启。
3. 打开 `http://<device-ip>/` 登录，在「数据面板」导入、校验并应用 bundle。
4. 也可先展开一个预设，再导入生成的设备原生 JSON：

   ```sh
   python3 firmware/tools/preset_tool.py list
   python3 firmware/tools/preset_tool.py export chart-kpi \
     --mock-host 192.168.1.20:8978 --output .tmp/chart-kpi.json
   python3 firmware/tools/preset_tool.py validate .tmp/chart-kpi.json
   ```

30 个预设和 TOKEN、天气、服务器、安全事件四个完整场景见
[预设库说明](firmware/presets/README.md)。真机使用 mock 时，`mock_host` 必须是
运行 mock 的电脑 LAN IP，不是设备自身的 `127.0.0.1`。

## 已实现的产品路径

- **数据面板**：card/chart/table/text/image/button/alert 七类 panel；30 个起点；
  标准 JSON、声明式 JSONPath、Prometheus、文本和 PNG/JPEG；变量、阈值、告警
  surfaces、linked panel、时间窗口和独立 source/panel 调度。
- **HTTP 配置中心**：中英管理页、Bearer 登录、草稿保护、schema 校验、导入导出、
  revision 冲突检测、source test 和原子应用；复杂转换由用户服务器脚本完成。
- **最终输入模型**：触控 tap/swipe、Up/Down/Front 手势、固定 Power/Home 安全槽位、
  最多 24 条稀疏自定义绑定；危险操作采用 5 秒内同一控件 fresh hold ≥700ms，
  本地恢复和 BLE 操作只接受实体 Power。
- **整机设置**：语言、WiFi、电源、前光、数据/EPD 刷新、RTC/时区、SD、诊断、
  网络恢复和工厂恢复；DeviceSettings typed API 与本机设置共用校验和热生效。
- **BLE**：初始化后的本地 PoP/Security 1 配网，以及默认关闭的被动 manufacturer/
  service observer；窗口运行并释放资源。持续手机 GATT、Bluetooth Classic、音频和
  通用蓝牙上网不在首版。

完整字段、API、交互、限制和中英使用流程以
[固件使用指南](firmware/README.md)为准。

## 当前限制与验收口径

- 首版不提供远程 OTA；深睡时 HTTP/BLE 均不可实时访问，电池模式只在唤醒窗口工作。
- 运行层支持 `auth_token_ref` 的 NVS 引用解析，但当前没有公开 UI/API/串口把任意
  数据源 token 写入 NVS 的完整流程，Bearer 认证链不能标为已验收。
- 普通自定义 headers 只能放非秘密值。敏感 headers 虽在 API 响应中掩码，当前仍会
  原值持久化到 SD；在修复前不得用它们保存秘密。服务器响应也应最小化，避免下发凭据。
- 最终基线已通过主机静态/mock/同源 C++/DOM 检查和完整 ESP-IDF 构建，包括 30 个
  预设与四个场景；**这些不是实体设备验收**。显示、触控、电气、真实网络、BLE/RF
  共存、功耗和长期运行仍须按[分类真机清单](firmware/README.md#真机验收清单当前全部未执行)
  验证，并绑定实际刷入镜像哈希。

## 文档导航

先读：

1. [固件完整中文指南](firmware/README.md)
2. [English integrated guide](firmware/README.md#english-integrated-guide)
3. [30 个预设与四个场景](firmware/presets/README.md)
4. [仓库文档导航](docs/README.md)

项目治理：[贡献指南](CONTRIBUTING.md)、[安全政策](SECURITY.md)、
[支持入口](SUPPORT.md)、[变更记录](CHANGELOG.md)、
[第三方声明](THIRD_PARTY_NOTICES.md)。

## 旧 Arduino 硬件参考（非主产品）

`src/`、`lib/`、`examples/` 和大部分 `docs/` 属于旧 Arduino/PlatformIO 硬件模板。
它们用于查阅引脚、总线、驱动、显示安全区、诊断和安全恢复，不代表新固件的架构或
实机验收状态。旧模板的独立版本为 1.0.18；不要与新固件镜像或版本混用。

- [旧硬件参考](docs/HARDWARE_REFERENCE.md)
- [旧驱动与 API](docs/DRIVER_GUIDE.md)
- [旧架构与总线所有权](docs/ARCHITECTURE.md)
- [旧示例](examples/README.md)
- [旧诊断指南](docs/DIAGNOSTICS_GUIDE.md)
- [旧模板实机验证范围](docs/VALIDATION.md)
- [显示安全区](docs/SAFE_AREA.md)
- [备份、刷写与恢复参考](docs/RECOVERY.md)

旧模板自身的完整回归（同时也会编译其诊断固件和五个示例）：

```sh
python3 scripts/validate_project.py
```

只做仓库静态结构、文本和 Markdown 链接检查：

```sh
python3 scripts/validate_project.py --quick
```

## 项目结构

```text
firmware/                 主产品：ESP-IDF 告警/数据面板
  components/             dashboard、pipeline、admin、BLE、设置与硬件组件
  main/                   应用、运行时、输入和恢复组合根
  presets/                30 个主机预设与四个完整 bundle 示例
  tools/                  校验、预设展开、mock 和串口 smoke 工具
src/  lib/  examples/     旧 Arduino 硬件模板与示例（参考）
docs/                     产品导航及旧硬件/驱动/恢复资料
scripts/                  仓库校验、旧模板打包与安全工具
```

`.tmp/`、`.pio/`、`release/` 和 `captures/` 是生成目录，不应提交。构建、校验和
打包命令不会自动刷写设备；发布二进制应通过 Release assets 分发。

## 许可证与来源

旧 Arduino 模板按根目录 [MIT License](LICENSE) 发布；`firmware/` 新固件是
[GPL-3.0-or-later](firmware/LICENSE) 衍生作品。来源、商标、提取数据和第三方边界见
[NOTICE.md](NOTICE.md)、[固件 NOTICE](firmware/NOTICE.md)及各自第三方声明。
项目与 EEGO、Xteink、CrossLink 及其权利人没有隶属或背书关系。
