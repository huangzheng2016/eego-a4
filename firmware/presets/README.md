# Dashboard 预设

本目录提供 30 个 panel/layout 预设和 4 个完整示例。它们只使用固件现有的
`eego.dashboard/2` 字段，不包含专用业务逻辑。

**预设不是设备直接接受的格式。** `eego.dashboard-preset/1` 需先展开为
`eego.dashboard-bundle/1`；`examples/*.json` 已经是可导入的完整 bundle。

```sh
python3 firmware/tools/preset_tool.py list
python3 firmware/tools/preset_tool.py export chart-kpi \
  --mock-host 192.168.1.20:8978 --output .tmp/chart-kpi.json
python3 firmware/tools/preset_tool.py validate .tmp/chart-kpi.json
python3 firmware/tools/preset_tool.py validate-library
```

布局遵循设备规则：每屏 8 个 grid 行，可见 panel 不重叠、不跨屏；列按
`grid_columns` 比例映射。`validate-library` 会检查这些规则以及内置样板 dashboard。

预设库同时内置在固件里（`GET /api/v1/dashboard/templates`）。修改本目录后运行
`python3 firmware/tools/preset_tool.py embed` 重新生成
`components/admin/src/dashboard_templates.inc`；过期时 `validate-library` 失败。

`--mock-host` 只写演示地址，不带 scheme、路径或凭据。真机必须改为电脑的 LAN
地址，不能使用设备自身的 `127.0.0.1`。接入真实服务时，同时修改 dashboard source
与对应 plan；需要认证时先按[数据源凭据](../README.md#数据源凭据)写入 NVS，
再选择 `nvs:source/<key>` 引用。

运行 `preset_tool.py list` 查看全部预设。指标、图表、表格、告警、文本、图片、
按钮和组合布局都只是现有 panel 的配置：

- 多指标 card 依赖同页 linked cards 和独立 metric 序列。
- 状态格、热力图和时间线读取 metric samples，不读取事件对象。
- `alert-count` 读取服务器提供的 `alerts.active`，设备不会自动聚合事件。
- `key-value` 和 `log-tail` 使用服务器预格式化文本，不在设备上解析日志。

## 完整示例

| 文件 | 场景与 mock 端点 |
|---|---|
| `examples/token-usage.json` | TOKEN 用量，`/token` |
| `examples/weather.json` | 天气，`/weather` + `/assets/weather.png` |
| `examples/server-metrics.json` | 服务器，`/server` |
| `examples/security-events.json` | 安全事件，`/security` |

启动通用 mock：

```sh
python3 firmware/tools/mock_dashboard_server.py --host 0.0.0.0 --port 8978
python3 firmware/tools/mock_dashboard_server.py --self-test
```

mock 另提供 `/metrics`、`/events`、`/text`、`/window` 和测试图片。标准 JSON 使用
`metrics[]`、`events[]`、可选 `text` 和 `reset_at`。mock 不需要真实凭据。

导入、编辑、凭据和刷新规则见[固件指南](../README.md)。
