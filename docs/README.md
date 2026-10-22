# Documentation

当前主产品是 `firmware/` 下的 ESP-IDF 可配置告警/数据面板。第一次接触项目时，
先读产品指南和预设库；旧 Arduino 文档仅用于硬件、驱动和恢复参考。

## 主产品：可配置数据面板

1. [固件完整使用指南](../firmware/README.md)：首次开机、AP/本地 BLE 配网、认证、
   HTTP 数据源、声明式解析、变量、布局、告警、刷新、输入、设置和恢复。
2. [English integrated guide](../firmware/README.md#english-integrated-guide)：
   同一最终基线的英文核心流程与限制。
3. [30 个预设与四个场景](../firmware/presets/README.md)：panel/layout 起点、
   TOKEN/天气/服务器/安全事件、mock 和导出/导入格式。
4. [仓库中文入口](../README.md) / [English entry](../README.en.md)：
   构建、快速开始、当前边界和验收口径。

## 验收与限制

- [主机证据与分类真机验收清单](../firmware/README.md#本次主机与构建证据不是实体设备验收)：
  区分静态、mock、同源 C++、DOM、构建与实体设备结果；列出显示、输入、HTTP、
  WiFi/BLE、SD/RTC/前光、深睡功耗和长期运行的待验项目。
- [旧 Arduino 模板实机验证摘要](VALIDATION.md)：**仅适用于旧模板**，不得作为新
  ESP-IDF 数据面板的真机验收证据。
- [安全刷写与恢复参考](RECOVERY.md)：备份、分区和哈希流程；旧镜像命令不能原样
  用于新固件。

## 旧 Arduino 硬件与驱动参考（非主产品）

以下资料属于旧 Arduino/PlatformIO 模板，用于硬件分析和底层开发，不描述新固件的
数据面板架构或验收状态。

### 开发与架构

- [从零开发指南](GETTING_STARTED.md)：旧模板安装、构建、备份和第一个示例。
- [可编译示例](../examples/README.md)：五个旧硬件外设示例。
- [驱动与 API](DRIVER_GUIDE.md)：旧显示、触控、SD、RTC、电池、无线和深睡接口。
- [项目架构](ARCHITECTURE.md)：旧分层、总线所有权、坐标和刷新模型。

### 硬件事实

- [逆向硬件参考](HARDWARE_REFERENCE.md)：证据来源、引脚、控制器和置信度。
- [官方/CrossLink/模板对照](OEM_CROSSLINK_MAPPING.md)：旧实现选择理由。
- [安全显示区域](SAFE_AREA.md)：R60 外轮廓、R48 内容区与矩形 UI 合同。

### 诊断与调试

- [旧诊断固件指南](DIAGNOSTICS_GUIDE.md)
- [Framebuffer 实机回读](FRAMEBUFFER_DEBUGGING.md)
- [常见问题](TROUBLESHOOTING.md)

## 维护项目

- [维护者与发布指南](MAINTAINER_GUIDE.md)
- [贡献规范](../CONTRIBUTING.md)
- [安全政策](../SECURITY.md)
- [变更记录](../CHANGELOG.md)

## 文档约定

- `已验证`：必须注明是实体 EEGO A4、可重复主机检查还是可重复构建，以及对应版本。
- `二进制确认`：由已记录 SHA-256 的固件分析得到，但未必覆盖所有硬件版本。
- `推断`：有支持证据，仍需新设备或仪表验证。
- `未知/不存在证据`：不得据此增加猜测性驱动或产品承诺。

文档中的 `<PORT>`、`<project>` 和 `<backup>` 都是占位符，不应原样执行。
