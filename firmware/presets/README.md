# Dashboard 预设库

本目录提供 30 个可选择的 panel/layout 起点和 4 个完整示例。预设使用现有
`eego.dashboard/2` 字段，不增加固件 panel 类型，也不包含 TOKEN、天气等专用业务逻辑。

**预设文件不是设备直接接受的格式。** `eego.dashboard-preset/1` 只在主机侧使用；
用 `preset_tool.py export` 展开为 `eego.dashboard-bundle/1` 后，再经 HTTP 编辑器或
API 校验、导入。`examples/*.json` 已经是可直接导入的完整 bundle。

## 选择与展开

```sh
python3 firmware/tools/preset_tool.py list
python3 firmware/tools/preset_tool.py export chart-kpi \
  --mock-host 192.168.1.20:8978 --output .tmp/chart-kpi.json
python3 firmware/tools/preset_tool.py validate .tmp/chart-kpi.json
python3 firmware/tools/preset_tool.py validate-library
```

`--mock-host` 只写演示变量默认值，不带 scheme、路径或凭据。接入真实服务时，在
HTTP 编辑器中同时修改 source URL 和对应 source plan；编辑器的 source 表单会保持二者
连接字段同步。不要把真实 token 写进这些 JSON。

## 30 个预设

| 分类 | 预设 ID | 现有渲染/动作模型 |
|---|---|---|
| 指标 | `metric-single` | 大数字 card、阈值、上一采样差值 |
| 指标 | `metric-dual` | `card.variant=dual` + 同页一个 linked card |
| 指标 | `metric-triple` | `card.variant=triple` + 同页两个 linked cards |
| 指标 | `progress` | `card.variant=progress`，0–100 进度条 |
| 图表 | `line-chart` | `chart.kind=line`，窗口序列、点、图例 |
| 图表 | `area-chart` | `chart.kind=line` + `fill=true` |
| 图表 | `bar-chart` | `chart.kind=bar` |
| 图表 | `sparkline` | `chart.kind=sparkline`，无图例紧凑折线 |
| 数据 | `table` | metric 行、labels、值和时间戳列 |
| 数据 | `key-value` | `text.variant=key_value`；服务器预格式化键值文本 |
| 数据 | `log-tail` | `text.variant=log_tail`；服务器预格式化多行日志 |
| 数据 | `event-stream` | 原生 alert 事件流，活动优先、其余按时间倒序 |
| 告警 | `alert-count` | 大数字 card 读取 `alerts.active` 指标；不是新增聚合器 |
| 告警 | `alert-banner` | 两行高 alert panel，作为紧凑横幅 |
| 告警 | `alert-table` | 全高 alert panel，严重度/标题/状态行 |
| 状态 | `status-grid` | `table.variant=status_grid`，阈值决定灰阶 |
| 状态 | `heatmap` | `table.variant=heatmap`，数值灰阶矩阵 |
| 状态 | `timeline` | `table.variant=timeline`，带时间戳的 metric 状态点 |
| 内容 | `text-single` | 单行动态文本 |
| 内容 | `text-multiline` | 多行动态文本 |
| 内容 | `image` | image panel + image asset + `response=image` |
| 内容 | `image-text` | 图片与动态说明文字复合布局 |
| 动作 | `button` | 单按钮数据刷新 |
| 动作 | `button-group` | 刷新、页面跳转、变量设置、二次确认 ack |
| 布局 | `chart-kpi` | 三个 KPI + 全宽趋势图 |
| 布局 | `chart-alerts` | 趋势图 + 告警流 |
| 布局 | `kpi-2x2` | 四个独立指标的 2x2 卡片 |
| 布局 | `three-row-stack` | 摘要、趋势、表格三排信息栈 |
| 布局 | `comparison` | 当前/前期 KPI 与双图对比 |
| 布局 | `mosaic` | 图、KPI、图片、文本、表格、告警六区马赛克 |

多指标预设的 linked cards 默认 `visible=false`，它们不是额外面板类型；主 card 会读取
同页独立 `source_id + metric` 序列。状态格/热力图/时间线都读取 metric samples，不要误把
事件对象塞给 table。告警计数需要服务器提供计数指标，设备不会遍历事件来自动生成该指标。

## 四个完整示例

| 文件 | 场景与默认 mock | 内容 |
|---|---|---|
| `examples/token-usage.json` | TOKEN 用量，`/token` | 用量、剩余进度、5 小时窗口、明细、阈值规则 |
| `examples/weather.json` | 天气，`/weather` + `/assets/weather.png` | 温湿度、风、温度窗口、天气图片 |
| `examples/server-metrics.json` | 服务器，`/server` | CPU/内存 KPI 与窗口、状态格、明细、CPU 规则 |
| `examples/security-events.json` | 网络安全，`/security` | 活动事件/拦截/失败登录指标、事件流、刷新与 ack |

所有示例的 `mock_host` 默认是 `127.0.0.1:8978`，仅供主机侧联调。真机必须改为运行
mock 的电脑 LAN IP（或改成真实服务 URL）；设备上的 127.0.0.1 是设备自身。

通用 mock 启动与契约检查：

```sh
python3 firmware/tools/mock_dashboard_server.py --host 0.0.0.0 --port 8978
python3 firmware/tools/mock_dashboard_server.py --self-test
```

除四个场景端点外，还有 `/metrics`、`/events`、`/text`、
`/window?points=24&hours=5`、`/assets/status.png`。JSON 端点使用设备现有的
`standard_json` 结构：`metrics[]`、`events[]`、可选 `text` 和 `reset_at`；metric 记录含
`metric/value/unit/timestamp_ms/labels`。窗口端点返回有序时间戳和窗口元数据。日志端点
只记录方法、无查询字符串的路径和次数，不记录请求头；mock 不需要也不应收到真实凭据。

完整配置流程、HTTP API、设备交互和资源预算见 [firmware/README.md](../README.md)。
