# EEGO A4 可配置告警/数据面板固件

[English integrated guide](#english-integrated-guide) · [30 个预设与四个场景](presets/README.md) ·
[仓库入口](../README.md)

EEGO A4（ESP32-S3 + 7.8" UC8279C 墨水屏）的可配置数据面板固件：纯 ESP-IDF
（无 Arduino 依赖），以 HTTP 数据源、声明式解析、变量、页面/panel、告警规则和
独立刷新组成完整仪表盘，并整合告警、WiFi/BLE、前光、电源、存储、诊断和恢复。
设备不执行 Python/JavaScript 等任意脚本；复杂业务解析应由用户服务器完成。

> **验收口径**：本指南描述最终集成基线的实现，不代表已在实体设备验收。主机 mock、
> 设备同源 C++、DOM、静态检查和完整构建与真机结果严格分开；未上机项目集中列在
> 文末清单。根目录旧 Arduino 模板的实机记录不转移到本固件。
>
> 许可：本目录代码为 GPL-3.0-or-later 衍生作品（移植自 eego-a4-weread，GPL-3.0；
> 板级硬件事实来自 eego-a4-template-firmware，MIT）。第三方组件许可见
> [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)，来源与商标边界见
> [NOTICE.md](NOTICE.md)。

## 构建与校验

前提：ESP-IDF v5.5.1（`IDF_PATH` 已配置）。目标芯片 ESP32-S3（16MB Flash +
Octal PSRAM，EEGO A4）。从仓库根目录执行：

```sh
python3 firmware/tools/validate_firmware.py
python3 firmware/tools/preset_tool.py validate-library
idf.py -C firmware -B .tmp/firmware-build build
idf.py -C firmware -B .tmp/firmware-build size
idf.py -C firmware -B .tmp/firmware-build size-components
```

- 分区表 `partitions.csv`：16MB A/B OTA（app0/app1 各 0x6B0000）+ NVS +
  otadata + spiffs + coredump；已启用 bootloader 回滚保护。**预留 OTA 分区不等于
  开放远程 OTA**。
- PSRAM 只经 `heap_caps_malloc(MALLOC_CAP_SPIRAM)` 显式分配；普通 malloc
  不触碰 PSRAM（避免 WiFi/NVS 内部分配到 PSRAM 导致 flash 写崩溃）。
- `validate_firmware.py` 做依赖方向、必需文件、禁止残留、许可、schema 和预设
  静态检查；它不编译、不刷写，也不能替代设备端导入校验或真机验收。

## 刷写（仅 USB，安全实践）

```sh
cd firmware
idf.py -p /dev/cu.usbmodem* flash monitor     # Linux 为 /dev/ttyACM*
```

- **固件不提供任何远程 OTA/固件上传端点**（`/api/v1/firmware/*` → 404）；
  刷写只走 USB。未来若开放 OTA，必须先有签名校验和 A/B 回滚。
- 刷写前按仓库 `docs/RECOVERY.md` 的备份/分区/哈希工作流操作；该文档中的旧
  Arduino 包命令只作安全参考，镜像必须换成当前 ESP-IDF 构建产物。刷写后用
  `python3 -m esptool verify-flash` 校验。
- 管理 API 状态端点暴露 `fw_version` + `fw_sha256`，验收结论必须绑定实体机
  实际运行的镜像哈希。

## 首次开机、AP 配网与管理认证

设备**无管理密码**时上电进入初始化配网模式：

1. 设备开放 AP「EEGO-A4-Setup」（固定 SSID，不含设备唯一标识）+ APSTA。
2. 手机/电脑连上该 AP，浏览器访问 `http://192.168.4.1/`：先扫描并选择 WiFi、
   输入密码，再**强制设置管理密码**（≥8 位，两次一致）。
3. 提交后设备以 APSTA 试连 STA（15s 超时）：成功才把 WiFi 凭据和管理密码的
   PBKDF2-HMAC-SHA256 哈希写入 NVS，随后关门户并重启；失败则显示原因并可重试。
4. 门户 10 分钟无操作自动关闭，之后需从本机设置重新打开限时恢复 AP。
5. 正常模式访问 `http://<device-ip>/`，用管理密码登录。IP 可由串口 `net` 或
   路由器 DHCP 列表查看；浏览器令牌只存 sessionStorage，设备端会话仅在 RAM。

**首次设置管理密码不能由 BLE 代替。** 本地 BLE 配网用于已初始化设备的配网/换网：
在本机「设置 → BLE 配网 / PoP」首次激活只布防，5 秒内重新按住**实体 Power**
至少 700ms 才开启 180 秒窗口。窗口显示 8 位 PoP，手机使用支持 ESP BLE
Provisioning 的客户端，以 Security 1 连接 `EEGO-A4-XXXX`（XXXX 为 MAC 末两字节）
并输入 PoP。它只写 WiFi 凭据，不设置管理密码；成功、失败、取消或超时会关闭窗口
并释放 BLE。恢复 AP 与 BLE 配网不会同时开放，observer 会让位于配网。

忘记管理密码只能全量恢复出厂，**不留后门**：本机设置页先激活恢复项，再在 5 秒内
重新按住实体 Power 至少 700ms；或在已知旧密码时远程调用
`POST /api/v1/system/factory-reset`，提交密码 + `confirm:"RESET"`。WiFi-only
重置不清管理密码，不能用于找回密码。

## 局域网管理

正常模式 STA 拿到 IP 后，管理入口 `http://<device-ip>/`（设备 IP 经串口
`net` 命令或路由器 DHCP 列表查看）：

- `POST /api/v1/auth/login {"password"}` → `{token, token_type:"Bearer",
  expires_in}`；之后请求头带 `Authorization: Bearer <token>`。
  登录限速：每 IP 5 次失败锁 5 分钟 + 全局 10 次/分钟（429 + Retry-After）。
- 主要端点（全部要 Bearer，除 login 与告警提交）：

| 端点 | 说明 |
|---|---|
| `GET /api/v1/status` | 设备/网络/内存/存储/告警计数/安全元数据（不含秘密值） |
| `GET /api/v1/config` / `PUT` | 配置读取（secrets 掩码）/ 白名单校验 + 原子持久化 |
| `GET /api/v1/config/schema` | 配置 schema v1 静态描述 |
| `GET /api/v1/settings` / `PUT` | DeviceSettings 读取 / typed 局部修改、原子持久化并热生效；含 revision、能力与电源状态 |
| `GET /api/v1/settings/schema` | 本机设置字段、范围和只读元数据 |
| `GET /api/v1/alerts` / `GET /alerts/{id}` | 告警列表 / 详情 |
| `POST /api/v1/alerts/{id}/ack` `/clear`、`POST /alerts/ack-all` | 确认 / 清除 |
| `GET /api/v1/types` / `PUT /api/v1/types/{id}` | 告警类型注册表查询 / 覆盖 |
| `POST /api/v1/sources/test` | 出网测试源（限速，不回显目标 body） |
| `GET /api/v1/sources/presets` / `GET /sources/presets/{id}` | 来源预设查询（id/中英名称/接入方式/字段映射模板/中英能力说明；未知 id 404） |
| `POST /api/v1/config/wifi` / `DELETE` | WiFi 账号增删（密码只入 NVS） |
| `POST /api/v1/system/reboot` `/factory-reset` | 重启（`confirm:"REBOOT"`）/ 恢复出厂（双确认） |
| `GET /api/v1/system/logs` | RAM 环形日志（安全事件，无 secrets） |

- 错误封装统一 `{"error":{"code","message","fields"?}}`；401 不区分无令牌/错
  令牌/错密码；Host/Origin 白名单抗 DNS rebinding；无 CORS 头；
  `Content-Type: application/json` 强制（415 否则）。
- 传输为局域网 HTTP 明文（首版决策；HTTPS 列为待决策项）；公网云不在首版范围。

## 告警提交（http_push）

`POST /api/v1/alerts`，头 `X-Ingest-Key: <key>`（与管理令牌独立的最小权限
凭据：只能提交告警，读任何资源一律 403）。ingest key 在创建/轮换时完整显示
一次，之后任何响应只含前 4 位掩码；限速每 key 30 次/分钟。

请求体为规范事件（M11 §4 契约）：

```json
{ "type": "home.water_leak", "severity": "critical",
  "title": "厨房水浸", "message": "传感器触发", "fields": {"room": "kitchen"} }
```

批量：`{"alerts": [ ... ≤32 条 ...]}`（批量算 1 次限速）。预算：单条 ≤16KB、
title ≤120、message ≤2000、fields ≤16 键；违反返回字段级 400。可选 `"state":
"cleared"` 显式清除、`"ttl_s"` 自定义存活秒数（上限 366 天）。

## 来源预设

net_stack 内置 7 个来源预设（`net_stack/source_presets.cpp`）。管理 UI「来源」
页可浏览/选择预设并一键「填入配置草稿」——草稿并入 `sources[]` 后自动跳到
「配置」页继续编辑，保存仍走 `PUT /api/v1/config` 原有校验，不绕过任何配置
检查。API 侧经 `GET /api/v1/sources/presets`（列表）与
`GET /api/v1/sources/presets/{id}`（单个，未知 id 404）查询，返回字段：
`id`、`label_zh`/`label_en`（中/英文名称）、`kind_hint`（推荐接入方式
http_poll/http_push）、`builtin_poll`（设备原生解析=true / 脚本桥接=false）、
`config_template`（SourceConfig 起点模板，http_push 预设为 null）、
`notes`/`notes_en`（中/英文能力说明与脚本输出契约）。管理 UI 按当前
zh/en 语言环境渲染预设名称与说明。预设只是 `SourceConfig` 起点，
改 url/token/interval 后校验规则与手写配置完全一致。

能力边界（不伪装支持）：

- **设备原生支持**（`builtin_poll=true`）：`prometheus`、`custom_json` ——
  模板开箱即可由固件直接解析。
- **脚本桥接支持**（`builtin_poll=false`）：`home_assistant`、`grafana` 经
  http_push 直推（面板侧无需 `sources[]` 项）；`nas_generic`、`openwrt`、
  `ups_nut` 需按 `notes` 的脚本契约自建桥接脚本产出规范事件数组——固件不
  直连 NUT/SNMP/ubus/各厂 NAS 私有 API。

| 预设 id | 接入方式 | 说明 |
|---|---|---|
| `home_assistant` | http_push | HA 无通用告警查询 API，不伪装轮询；自动化 REST Command/webhook 直推 |
| `grafana` | http_push | Contact point webhook；通知模板渲染成规范事件 |
| `prometheus` | http_poll（开箱可解析） | `/api/v1/alerts`，array_path/field_map/severity_map 内置 |
| `nas_generic` | http_poll（自定义脚本） | 各厂 NAS API 各异；脚本产规范事件数组，恒等映射 |
| `openwrt` | http_poll（自定义脚本） | ubus 无通用告警接口；脚本模板同上 |
| `ups_nut` | http_poll（自定义脚本） | NUT 文本协议不直连；upsc 轮询脚本阈值判断 |
| `custom_json` | http_poll（恒等映射） | 端点直接返回规范事件数组的通用兜底 |

通用 `SourceConfig` 字段：`id`（[a-z0-9_-]{1,64}）、`url`、`interval_ms`
（10s–1h，默认 60s）、`auth_token_ref`（token 存 NVS，`nvs:<key>` 引用）、
`tls_verify`（自签显式关闭）、`clear_after_misses`（默认 3，0=从不）、
`array_path`、`field_map`、`severity_map`、`clear_states`、`dedup_key`、
`headers`、`timeout_ms`、`max_body_bytes`（默认 64KB，解析预算，超限记源错误
不硬吃）。响应 2xx 原始 body 落 SD `cache/<source>.json`；连续 miss 达标自动
清除在册活动记录。

## 数据面板与预设库

完整数据面板使用 `eego.dashboard/2`；HTTP 持久化格式是
`eego.dashboard-bundle/1`（`dashboard + source_plans + revision`）。它沿用上述告警
系统，同时提供 card/chart/table/text/image/button/alert 七类现有 panel。
**30 个可选 panel/layout 的起点、四个完整场景和 mock 契约见
[presets/README.md](presets/README.md)。** 预设不是按 TOKEN/天气场景硬编码的固件分支；
四类示例只是通用指标、事件、文本和图片映射的实例。

### 从选择预设到设备生效

1. 启动演示源（真机联调需显式监听 LAN）：

   ```sh
   python3 firmware/tools/mock_dashboard_server.py --host 0.0.0.0 --port 8978
   ```

2. 选择并展开一个预设。将地址改为运行 mock 的电脑 LAN IP；不要保留真机不可用的
   `127.0.0.1`：

   ```sh
   python3 firmware/tools/preset_tool.py list
   python3 firmware/tools/preset_tool.py export chart-kpi \
     --mock-host 192.168.1.20:8978 --output .tmp/chart-kpi.json
   python3 firmware/tools/preset_tool.py validate .tmp/chart-kpi.json
   ```

   `eego.dashboard-preset/1` 和 `presets/index.json` 是主机侧目录/模板格式，
   **不能原样 POST 给设备**。export 后才是设备原生 bundle。四个
   `presets/examples/*.json` 无需展开，可直接导入；它们需要 mock 或替换为真实源，
   不承诺离线显示演示数据。

3. 浏览器登录 `http://<device-ip>/`，进入「数据面板」：导入 JSON → 校验 → 应用。
   导入只替换草稿，应用后才原子生效。编辑器可维护 dashboard、页面、panel、source、
   变量、告警规则和布局；高级 JSON 区可编辑 assets/samples/events。修改 source 时，
   编辑器同步对应 plan 的连接字段；手工编辑时必须自行保持 `id/kind/enabled/url/
   refresh_ms/timeout_ms/max_response_bytes/tls_verify/auth_token_ref/headers` 一致。
4. 在 source 表单先做「测试」。结果只含归一化指标/事件/文本/图片元数据，不回显原始
   响应。应用前运行主机全量检查：

   ```sh
   python3 firmware/tools/validate_firmware.py
   python3 firmware/tools/preset_tool.py validate-library
   ```

也可全走 HTTP API（均需 Bearer，写操作要求 JSON 和同源策略）：

| 端点 | 用途 |
|---|---|
| `GET /api/v1/dashboard/schema` / `sample` | schema 信息 / 内置样板 |
| `GET /api/v1/dashboard` / `export` | 读取当前 bundle / 导出；`format=canonical` 仅 dashboard |
| `POST /api/v1/dashboard/validate` | 只校验候选 bundle，不修改设备 |
| `POST /api/v1/dashboard` | 仅在未配置时创建；已配置返回 409 |
| `PUT /api/v1/dashboard`（或 `POST /import`） | 校验并原子替换整个 bundle |
| `DELETE /api/v1/dashboard` | 删除 dashboard，不影响其他管理配置 |
| `POST /api/v1/dashboard/sources/test` | 测试草稿 plan，返回安全归一化预览 |
| `/api/v1/dashboard/pages...`、`/sources...`、`/variables...`、`/alerts...` | 对应资源的 GET/增删改 |

写操作带 `If-Match: <revision>`；首次/强制流程可按 API 支持使用 `If-Match: *`，但多客户端
编辑应读取最新 revision，过期写返回 409，不能靠覆盖避免冲突。典型替换：

```sh
curl -H "Authorization: Bearer $EEGO_TOKEN" \
  -H 'Content-Type: application/json' --data-binary @.tmp/chart-kpi.json \
  http://192.168.1.30/api/v1/dashboard/validate
curl -X PUT -H "Authorization: Bearer $EEGO_TOKEN" -H 'If-Match: *' \
  -H 'Content-Type: application/json' --data-binary @.tmp/chart-kpi.json \
  http://192.168.1.30/api/v1/dashboard
```

#### HTTP headers 与 NVS secret 引用（当前限制）

- dashboard source 和对应 plan 都必须保存相同的连接字段。`headers` 只放**非秘密**值；
  `Authorization`、`Host`、`Content-Length` 由运行层保留并拒绝手工配置。不要把管理
  token、数据源 token 或其他秘密写进 preset、bundle、mock、命令历史或日志。
- 运行层能把 `auth_token_ref: "nvs:<key>"`（默认 `sec` 命名空间）或
  `nvs:<ns>/<key>` 解析为临时 `Authorization: Bearer ...`，bundle 只应保存引用。
  **但最终集成基线没有公开 UI/API/串口把任意数据源 token 写入 NVS 的流程**；现有
  内部写入只覆盖管理密码哈希和 ingest key。因此 Bearer 的“写入 secret → 引用 →
  在线测试”尚未形成用户可完成的闭环，不能标为已验收。
- 普通自定义 headers 也不是安全的替代品：API 读取会把敏感值掩码，但当前 dashboard
  持久化会把它们原值写入 SD `dashboard.json`。在此问题修复前，禁止把秘密放入
  `X-API-Key`、Cookie 等普通 headers；旧告警 `sources[].headers` 同样只放非秘密值。

### 数据映射与服务器脚本边界

- `standard_json` 直接读取 `metrics[]`、`events[]`、可选 `text`；`metrics_json` 和
  `events_json` 只取对应集合。`generic_json` 用声明式 JSONPath 映射异构 JSON；
  `prometheus` 读取现有 scalar/vector/matrix 响应；`text` 和 `image` 处理原始 body。
- metric 的标准字段为 `metric/value/unit/timestamp_ms/labels`，时间戳可为秒、毫秒或
  ISO-8601；事件字段为 `id/rule_id/timestamp_ms/severity/state/title/message/
  linked_panel_id`。面板用 `source_id + metric` 绑定序列；chart/table 必须绑定，双/三
  指标 card 还需同页 linked cards 和不同序列。
- `time_window` 使用 `now`、`now-15m`、`-1h` 或带 `ms/s/m/h/d/w` 的时长。窗口历史
  可聚合 sum/avg/min/max/delta/rate；`value_expression` 和告警 expression 只允许现有
  白名单表达式（算术、比较、条件和 convert/min/max/clamp 等），不是通用脚本。
- 复杂 OAuth 流程、分页、嵌套 API 聚合、日志格式化、键值提取、业务告警计数和图片
  生成应放在**用户服务器脚本**。设备不嵌入 Python/JavaScript 运行时，不执行预设或
  响应中的代码。变量 `${name}` 仅做单次插值，`$${name}` 输出字面值，变量内容不会
  再被扫描成表达式。
- `alert-count` 读取源提供的 `alerts.active`；`key-value`/`log-tail` 读取源预格式化
  文本；面积图只是 `line + fill`；状态格/热力图/时间线读取 metric samples。不要假设
  设备会把日志解析成指标、把事件自动聚合成计数或支持 Grafana 全部 transform。
- 图片使用现有 PNG/JPEG 灰度解码；GIF/SVG 不在支持范围。网络图片 plan 的
  `response=image` 与 dashboard asset URI 对应，SD 图片用 `sd:` 或无 `..` 的绝对路径。

### 面板、布局、阈值与告警链接

按「source → source plan → 变量 → 页面/panel → 告警规则」顺序编辑并随时校验：

- 页面选择 `grid_columns/row_height`，每个 panel 设置 `grid{x,y,w,h}` 和 `visible`；
  card/chart/table/text/image/button/alert 共用同一布局模型。chart 设置 kind、点、填充和
  Y 轴；table 配置列；image 绑定已注册 asset；button 配置封闭 action。
- panel 的 `thresholds[{value,severity,label}]` 控制数值严重度样式；`style` 控制单位、
  精度、variant、线宽、图例和反色。`linked_panel_ids` 用于双/三指标 card、chart carousel
  或事件关联，必须引用实际存在且满足类型约束的 panel。
- `alert_rules[]` 绑定 `source_id + metric + expression`，可设 `for_ms`、消息、严重度、
  `surfaces`（banner/card/table_row/sparkline）及 `style.linked_panel_id`。它是声明式规则，
  不是脚本，也不会把事件自动聚合为指标。每次修改先「校验」，再「应用」原子生效。

### 设备交互、危险确认与自定义输入

- 触控抬起才结算 tap；按钮直接命中对应 action，alert 行可聚焦并跳到 linked panel，底部
  左右区域和水平 swipe 翻页。Power 短按激活当前控件、长按返回；有可轮换 chart 时，
  短按 Power 可切换关联图表。变量通过 `set_variable` 动作切换为指定值。
- 最终默认映射：Up/Down **短按移动焦点、长按翻页**；Front（屏下键）**短按 Home**、
  **长按 Help**、**双击 Status**。Up/Down 双击和 Up+Down chord 默认不执行隐藏动作，
  只在有自定义绑定时生效。Power 激活/返回/唤醒与 Front Home 是固定安全底座。
- action 仅支持 `goto_page/goto_panel/set_variable/refresh/back/ack_alert`。`confirm=true`
  或 ack 首次激活只布防；必须在 5 秒内从**同一控件重新按下并持续至少 700ms**。布防时
  已有的按压不算，移动/其他输入/超时/页面或快照变化会取消；执行前重新校验目标。
  这不是“再点一次”。网络恢复、WiFi 重置、恢复出厂、BLE 配网/解绑还额外限定只能
  用**实体 Power fresh hold**，触屏和 Front 均不能确认。
- `refresh` 的 `all/current/<source>` 只安排**数据轮询**，不等于强制墨水屏全刷。
- 自定义绑定经 `GET /api/v1/input-bindings/schema` 查看 9 个可配 trigger、已注册 action
  和页面；`GET /api/v1/input-bindings` 读取 revision/defaults，`PUT` 同路径提交最多
  24 条稀疏覆盖，`POST /api/v1/input-bindings/reset` 恢复默认。PUT/reset body 必须带当前
  `schema_version:1` 和 `revision`，冲突返回 409。精确页面 → `dashboard` 分组 → `*` →
  默认依次解析；`action.kind` 仅 `none/builtin/action`，dashboard action 只能引用已注册
  `panel_id + action_id`，不允许 URL、HTTP、脚本或未注册变量。`power.*`、`front.short`、
  确认模态和 recovery 不可映射；本机「设置 → 重置输入映射」也可恢复默认。

示例（把全局 Front 双击改为打开帮助；`none` 可用于禁用低优先级默认）：

```json
{"schema_version":1,"revision":1,"bindings":[
  {"page":"*","trigger":"front.double",
   "action":{"kind":"builtin","target":"help"}}
]}
```

### 数据与墨水屏刷新语义

- DeviceSettings 的 `data_refresh_s` 是 net_task 告警 `http_poll` sources 的统一周期覆盖：
  `0` 恢复各 source 自己的 `interval_ms`，非零可取 30–86400 秒并同时约束失败退避。
  dashboard 的 `source.refresh_ms` 与 panel 覆盖仍按 bundle 独立调度，不被这个告警源
  全局值隐式改写。`epd_full_refresh_s` 则是独立的墨水屏周期全刷：`0` 表示仅手动，
  非零可取 3600–604800 秒；它与数据轮询频率无关。
- panel 数据/文本/图片或状态变化按 fingerprint 标脏，未变化可跳过绘制；结构、翻页和
  布局变化会触发更大的重绘。面板周期可覆盖为配置默认或 1/5/15/30 分钟，运行层实际
  panel 调度最小 1 分钟；周期在编辑器/bundle 中配置。电子纸不适合秒级动画，建议普通源
  1–5 分钟、天气 5–15 分钟、图片 15 分钟以上，并按信息价值减少全屏变化。
- 电池模式只在唤醒/交互窗口联网轮询，不能宣传实时推送；常电模式按快照和 dirty panel
  更新。离线与 stale 状态不等同于清空最后缓存，source 健康需在真机核对。

### 配置、内存与渲染预算

以下是**硬上限/配置上限，不是实测剩余 RAM 或性能保证**；主机校验只能拒绝超限配置。

| 项目 | 上限/行为 |
|---|---|
| dashboard / HTTP bundle / 单个 source plan | 128 KiB / 192 KiB / 32 KiB；bundle JSON 深度 18，dashboard/plan 深度 16 |
| 集合 | 12 sources、24 variables、12 pages、24 panels/page、72 panels 总数、48 alert rules、24 assets |
| panel | 8 actions、16 thresholds、16 links、24 columns；文本 4096 code points、1–64 行（还受像素高度限制） |
| 配置快照与映射结果 | 2048 samples、256 events；每 plan 64 metric mappings、16 event mappings；映射记录最多 2048 |
| 单个 source 响应 | 默认 64 KiB，可配 1–256 KiB；超限失败，不静默截断 |
| 图表与状态 | 每 chart 最多 128 绘制点；status grid 32、heatmap 64、timeline 16、alert panel 256 条 |
| 历史 | 64 series、256 display points/series、8192 总 display points、1441 raw points/series、保留 8 天 |
| SD cache | history 16 MiB、单 source snapshot 512 KiB；写入采用临时文件 + rename |
| 网络图片 | 响应最大 256 KiB；UI 编码图片输入最大 512 KiB；PNG/JPEG，单边最大 4096 且总像素最大 1,048,576 |
| 图片缓存 | 最多 2 项、合计 512 KiB；PNG 还需整图 RGBA/灰度工作区，不能把编码大小当作峰值内存 |

屏幕布局映射为 4 个逻辑列、每屏 8 行；24 格 grid 只是声明便利，超出 8 行会分页。
预设均按单屏无重叠校验。多个大 PNG、多数据源长历史和高频全屏变化仍可能造成内存或
刷新压力，需用 `idf.py size`、串口 dashboard 诊断、内部 RAM 水位和真机刷新耗时验收。

## 本机设置、BLE 与恢复

DeviceSettings schema v1 统一保存非秘密本机偏好（语言、电源、前光、数据/EPD 刷新、
休眠、时区、诊断、BLE observer），迁移旧 NVS `ui` 与 `admin.display`。本机设置页和
`GET/PUT /api/v1/settings` 共用 typed 局部校验、原子持久化和订阅式热生效；
`GET /api/v1/settings/schema` 给出范围，`GET /api/v1/settings` 另返回 revision、硬件能力、
电源和 BLE 状态。硬件不存在时前光/BLE 项会禁用或拒绝修改，不伪装成功。前光开关、
亮度和色温持久化，入睡前在两个网络空闲等待都结束后显式关闭，唤醒按保存值恢复；
中止休眠会恢复前光。诊断开关控制串口命令分发，SD/RTC 状态以实际探测为准。

设置页提供限时网络恢复 AP、WiFi-only 重置和全量恢复出厂。首次激活只布防，三类操作
都必须在 5 秒内重新按住**实体 Power** 至少 700ms，触屏不能确认。WiFi-only 只清 WiFi
账号并打开恢复 AP，不清管理密码；忘记管理密码仍需全量恢复。全量恢复清除
`sec/wifi/ui/ble/nimble_bond/dashboard` 命名空间、DeviceSettings、input bindings 以及
config/alerts/dashboard 等明确列出的 SD 文件，保留字体和未知用户文件；SD 不可用或
任一清除失败都不会安排“成功”重启。

### BLE 配网、observer 与共存边界

- BLE 默认全部关闭。本地 BLE 配网流程见「首次开机」；它只由本机 fresh-hold 触发，
  使用每窗口随机 PoP + Security 1，180 秒后超时，不提供远程开启配网的 HTTP 端点。
  `POST /api/v1/settings/ble/unbind`（空 JSON `{}`）或本机解绑可清 bond；本机解绑仍需
  实体 Power fresh hold。
- observer 是**被动广播扫描**，不是手机持续 GATT 连接。HTTP 设置页或
  `PUT /api/v1/settings` 配置 `ble_observer_enabled/window_s/interval_s/dedup_ms/rules`，
  本机设置可开关；启用时至少一条合法规则。最多 4 条规则、每条 1–4 个数值字段、
  payload 前缀匹配最多 12 字节。扫描窗口 2–60s，窗口后的间隔 30–86400s 且不短于窗口，
  去重 0–60000ms。
- 规则按 manufacturer company ID 或 16/32/128-bit service UUID 二选一过滤，支持 payload
  offset/match 和 u8/i8/u16le/i16le/u32le/i32le/f32le + 非零 scale。匹配后生成带 TTL 的
  统一告警事件，数值进入 `fields`，并附 `device/rssi/rule`；不会执行响应代码，也不会
  自动变成 dashboard HTTP source 的 metric 序列。
- ESP32-S3 的 WiFi/BLE 共用射频，固件采用窗口扫描和角色互斥/资源释放；observer 让位于
  BLE 配网和恢复 AP，电池模式每次唤醒最多一轮，深睡前停止 BLE。`settings` 中的 dropped、
  duplicate、malformed、ingest_errors 和 DIRAM 水位用于现场诊断。**射频共存、手机兼容性、
  丢包率与功耗仍需真机验收**。
- 首版不包含 Bluetooth Classic、音频、通用蓝牙上网、持续手机 GATT 或远程 OTA；深睡
  期间 HTTP/BLE 均不可实时访问。

## 供电模式

设置页/串口 `status` 可见当前模式；`PowerMode` 三档由 DeviceSettings 持久化并热生效：

- **Auto（默认）**：按充电状态自动判定——充电中=常供电，否则=电池；无电池
  读数（台式）视为常供电，避免误判进休眠。
- **常供电（Mains）**：modem-sleep 常驻，WiFi 即时在线，http_push 即时到达，
  面板按快照刷新。
- **电池（Battery）**：按 `battery_poll_s` 周期执行唤醒 → 等 WiFi → 全源轮询 →
  静默 → 交互窗口 → 安全休眠（GPIO4 保持锁存）；`idle_sleep_s` 控制无操作窗口，
  也可配置本地时间睡眠计划。http_push 延迟到下一周期，深睡时 HTTP/BLE 均不可达；
  恢复 AP 窗口暂停自动入睡。

电池信息：电压/百分比/充电状态经 ADC 分压（÷1.559）读取，`battery` 串口
命令与状态页可见。

## 安全边界

- **刷写**：仅 USB；无远程 OTA/上传端点（一切未知路径 404）。
- **secrets**：WiFi 密码、管理密码哈希和 ingest key 只存 NVS（`wifi`/`sec`）；
  会话令牌 RAM only（重启即清，最多 3 个 LRU）。管理响应和串口日志应掩码/省略秘密，
  但**普通 headers 会明文写 SD 的已知限制不能以“响应已掩码”掩盖**。数据源 NVS 引用
  缺少公开写入流程，普通 headers 又不得存秘密，详见上文；在这两项闭环前，不宣称
  任意认证数据源或“所有 SD 文件无秘密”已验收。
- **串口（UART0 diag）**：视为物理可信（有 USB 接触即可整片刷写，加锁无
  意义），但 diag 命令只输出状态/计数/标识，不读取不打印任何凭据；
  `inject` 只经 net_task 命令队列入队。
- **局域网 HTTP**：明文 + Host/Origin 白名单 + 无 CORS + JSON Content-Type
  强制 + 登录/端点限速 + CSP（管理页告警内容一律 textContent 渲染，防存储型
  XSS 偷令牌）。
- **残余风险（记录在案）**：首版未开 flash encryption / secure boot v2——
  有物理 flash 读权限者可提取 secrets；是否启用列为待用户决策（影响
  RECOVERY.md 备份/读回工作流）。
- **许可边界**：复用 eego-a4-weread（GPL-3.0）代码，衍生作品整体
  GPL-3.0-or-later；weread 协议/登录/加密代码、boot_logo 商标素材、miniz
  **未复用/未采用**；细节见 NOTICE.md / THIRD_PARTY_NOTICES.md。

## SD 卡布局

```
/sdcard/alertpanel/config.json      # 非秘密管理配置；普通 headers 不得存秘密
/sdcard/alertpanel/dashboard.json   # bundle + revision；敏感 headers 当前会原值保存
/sdcard/alertpanel/alerts.db        # 告警库（JSONL，原子持久化）
/sdcard/alertpanel/cache/<src>.json # 告警轮询源 2xx 原始响应缓存
/sdcard/alertpanel/dashboard_cache/ # dashboard 历史、归一化快照和图片缓存
/sdcard/fonts/...                   # 可选 UI 字体（.eefont/.bin，配置 admin.display.font）
```

SD 未挂载时：告警库降级为内存运行（重启丢失），状态页如实显示；dashboard 持久缓存
不可用不能被报告为已保存。用户必须避免在 headers、URL、事件字段或服务器响应中下放
凭据；原始 2xx 响应会按设计缓存，服务器应只返回面板所需的最小数据集。

## 主机工具（firmware/tools/）

```sh
# 1) 静态自检：依赖/许可/partitions/sdkconfig/admin + dashboard schema、引用和预算
python3 firmware/tools/validate_firmware.py            # 任一 FAIL 退出码 1

# 2) 预设目录、单个 bundle 校验、整库校验与主机侧展开
python3 firmware/tools/preset_tool.py list
python3 firmware/tools/preset_tool.py validate .tmp/chart-kpi.json
python3 firmware/tools/preset_tool.py validate-library
python3 firmware/tools/preset_tool.py export metric-single --output .tmp/metric-single.json

# 3) 通用 dashboard mock：metrics/events/text/assets/window + 四场景
python3 firmware/tools/mock_dashboard_server.py --host 0.0.0.0 --port 8978
python3 firmware/tools/mock_dashboard_server.py --self-test

# 4) mock 告警源：8 场景（normal/batch/dup/clear/ttl/oversize/badlen/slow）
python3 firmware/tools/mock_alert_server.py --host 0.0.0.0 --port 8977
#   设备 sources[] 指向 http://<主机IP>:8977/<场景>；索引页 / 给出建议 SourceConfig

# 5) 真机串口 smoke（绝不刷写设备；需 pyserial）
python3 firmware/tools/smoke_serial_test.py --port /dev/cu.usbmodem* \
    --admin-password '<管理密码>' --mock-url http://<主机IP>:8977/normal
```

## 本次主机与构建证据（不是实体设备验收）

在最终集成基线上，以下检查已通过：`validate_firmware.py` 51 项；30 个预设 + 4 个
场景的 host 校验；34 个 bundle 的设备同源 C++ dashboard/source parser 与
DataPipeline configure/tick；mock loopback HTTP 11 项；输入状态机 71 项；
DeviceSettings/BLE observer schema 合法与非法边界；BLE 角色/安全/恢复源码检查；
typed settings + dashboard editor DOM 检查；完整 ESP-IDF v5.5.1 build/size/
size-components。应用 bin 为 0x2418e0 字节，分区余量 66%，DIRAM 剩余 186885 字节。
这些结果证明主机可复现路径和可构建性，不证明实体显示、射频、电气或在线设备行为。

## 真机验收清单（当前全部未执行）

上机前必须重新构建并记录刷入镜像 SHA-256；上机后逐项签字，不得沿用旧 Arduino 模板的
[实机验证记录](../docs/VALIDATION.md)。

### 启动、显示、字体与灰阶

- [ ] 冷启动/复位/唤醒均先保持 GPIO4，串口 `version/status` 与 boot 的 latch、touch、
      buttons、frontlight、rtc、sd、display 探测符合实机；无异常重启或 Brownout。
- [ ] Full/Fast/局部刷新、翻页和 dirty panel 更新正确；目视四级灰条 0/4/8/12 可辨，
      检查残影、对比度、圆角安全区和 30 个预设的实际排版。
- [ ] 中英文字、长文本/多行文本、表格、按钮、告警灰阶、PNG/JPEG 和大图回退正常；
      记录图片解码峰值、内部 RAM 最低水位与单次刷新耗时。

### 触控、实体按键与自定义输入

- [ ] tap 抬起结算、slop 取消、水平 swipe、Front short/long/double 与 Up/Down
      short/long/double/chord 符合最终默认；Power 激活/返回、Front Home 不可被覆盖。
- [ ] 调校 700ms long/fresh-hold、280ms double、140ms chord、24px slop、80px/1.2s
      swipe、750ms 动作时效，以及按键/触控轮询和唤醒 release gate；不得出现一次按压
      重复执行或晚到事件逆向触发。
- [ ] 每个危险动作都验证“布防 ≠ 执行”：同一控件全新按住 ≥700ms 才执行，旧按压、
      第二击、移动、其他输入、超时、页面/快照变化均取消；恢复和 BLE 操作只接受实体
      Power。验证 24 条绑定上限、revision 冲突、非法/过期 action、NVS 损坏回退和本地重置。

### 首次配网、HTTP 在线配置与告警

- [ ] 无密码首启出现 `EEGO-A4-Setup`，两步门户、错误密码重试、10 分钟关闭、重启联网
      和登录/登出/401/限速符合预期；管理密码不进入日志或 SD。
- [ ] 在线完成 30 个预设选择/导出/导入与 TOKEN/天气/服务器/安全事件应用；逐项核对
      source test、声明式 JSON/JSONPath/表达式、变量、grid、阈值、alert surfaces、
      linked panel、独立 source/panel 刷新、revision 409 和离线/stale/缓存恢复。
- [ ] 验证轮询 normal/batch/dup/clear/ttl/oversize/badlen/slow、http_push 单条/批量、
      ack/clear/mute 的触控/串口/API 一致性、重启后告警恢复和响应大小预算。
- [ ] 对认证源单独记录当前阻塞：通用 NVS token 写入流程缺失，不能验收 Bearer 全链；
      不将秘密放普通 headers，并检查 SD/导出/日志/缓存无新增秘密泄漏。

### WiFi/BLE 配网、observer 与射频共存

- [ ] 初始化后的本机 BLE 配网只在 fresh-hold 后开放，PoP/Security 1 与主流手机客户端
      兼容；成功、错误 WiFi、NVS 失败、取消、180s 超时均关闭并释放资源，且不与恢复
      AP 并存。BLE 配网不得绕过首次管理密码设置。
- [ ] manufacturer/service UUID observer 的过滤、offset/match、全部整数/f32 映射、
      去重、TTL、畸形包、队列溢出和事件字段正确；电池每次唤醒最多一轮，深睡前停止。
- [ ] 在 WiFi HTTP 长连接、轮询、BLE 配网和 observer 并行场景测量丢包、时延、断连、
      DIRAM 水位与恢复；不得把编译期 coexist 配置当作射频共存已验收。

### SD、RTC、前光与诊断

- [ ] SD 拔插/缺席/写满/只读时状态真实，告警可降级为内存；配置、bundle、history、
      图片与 alerts 原子写入及断电恢复正确，工厂恢复保留字体和未知用户文件。
- [ ] RTC 探测、VL/时间有效性、SNTP、时区 15 分钟步进、跨午夜睡眠计划和下一唤醒
      时间正确；诊断开关热生效，日志/串口不输出秘密。
- [ ] 有/无 LM3630A 机型均正确探测；前光开关、亮度、色温即时并持久生效，入睡前在
      网络空闲后关闭，中止休眠/唤醒按保存值恢复，缺席硬件拒绝假成功。

### 电源、深睡与长期运行

- [ ] Auto/Mains/Battery 切换、充电/无电池回退、轮询/交互窗口和定时/Power 唤醒正确；
      用仪表测量常电、窗口、深睡电流与唤醒耗时，深睡期间确认 HTTP/BLE 不可达。
- [ ] 电池下 HTTP 推送延迟到唤醒窗口，恢复 AP/BLE 窗口暂停自动入睡；反复验证前光
      关闭、GPIO4 锁存、唤醒 release gate 与 SD/NVS 状态。
- [ ] 至少 72 小时 soak：多源轮询 + 图片 + BLE 窗口 + 睡眠循环，无异常重启、看门狗、
      句柄/内存持续下降、SD 损坏或时间漂移；记录镜像哈希、日志和最低内部 RAM。

首版明确不做持续手机 GATT 和远程 OTA；flash encryption/secure boot v2 尚未启用。
除上述已列主机证据外，实体网络、硬件、电气、功耗与长期稳定性均为**未验证**。

# English integrated guide

This ESP-IDF firmware is the primary configurable alert/data-panel product for the
EEGO A4. It combines HTTP sources, declarative parsing, variables, layouts, alert
rules, independent refresh, local input, WiFi/BLE, frontlight, storage, diagnostics,
and power recovery. It does not execute arbitrary Python/JavaScript. Put OAuth flows,
pagination, complex aggregation, log formatting, and image generation in your own
server-side scripts. Legacy Arduino documents in this repository are hardware
references only; their physical-validation claims do not apply to this firmware.

## 1. Build, first boot, and sign-in

1. Install ESP-IDF 5.5.1. From the repository root run
   `python3 firmware/tools/validate_firmware.py`, then
   `idf.py -C firmware -B .tmp/firmware-build build`, `size`, and `size-components`.
   Flash only over USB after making a full backup and checking partitions and hashes.
   Remote OTA/upload is not provided in this release.
2. With no admin password, join `EEGO-A4-Setup` and open `http://192.168.4.1/`.
   Select WiFi, then set an admin password of at least eight characters. Credentials
   and the PBKDF2 password hash are committed only after the STA connection succeeds;
   the device then reboots. The portal closes after ten idle minutes.
3. Open `http://<device-ip>/` and sign in. Use the serial `net` command or DHCP list
   to find the IP. Browser tokens use sessionStorage; device sessions are RAM-only.
4. Local BLE provisioning is for an **already initialized** device, not a replacement
   for first-boot admin-password setup. In Settings → BLE setup / PoP, arm the action,
   then freshly hold physical Power for at least 700 ms within five seconds. Enter the
   displayed eight-digit PoP in an ESP BLE Provisioning client using Security 1.
   The 180-second window writes WiFi credentials only and closes on success, failure,
   cancellation, or timeout. It never runs concurrently with the recovery AP.

## 2. Select, import, and configure a dashboard

Start the credential-free mock on a LAN-reachable host, then export a host preset to
the device-native bundle format:

```sh
python3 firmware/tools/mock_dashboard_server.py --host 0.0.0.0 --port 8978
python3 firmware/tools/preset_tool.py list
python3 firmware/tools/preset_tool.py export chart-kpi \
  --mock-host 192.168.1.20:8978 --output .tmp/chart-kpi.json
python3 firmware/tools/preset_tool.py validate .tmp/chart-kpi.json
```

Use the mock computer's LAN IP, not `127.0.0.1`, on a physical device. The 30
`eego.dashboard-preset/1` files and catalog are host-side templates; export produces
`eego.dashboard-bundle/1`. The four files under `presets/examples/` are already bundles.
In the bilingual web UI open Dashboard → import JSON → validate → apply. Import changes
only the draft; apply validates and atomically replaces the active bundle. Equivalent
APIs include `/api/v1/dashboard/validate`, `PUT /api/v1/dashboard`, `/export`, resource
CRUD, and `/sources/test`. Send `If-Match: <current revision>` for multi-client edits;
stale writes return 409.

Configure in this order:

- **Connection:** give each HTTP source and its source plan the same id, URL, refresh,
  timeout, response budget, TLS flag, token reference, and headers. The form keeps these
  fields synchronized; manual JSON editing must preserve them.
- **Declarative parsing:** use `standard_json`, `metrics_json`, `events_json`,
  `generic_json` with bounded JSONPath mappings, `prometheus`, `text`, or `image`.
  Expressions are a whitelist for arithmetic, comparison, conditionals, and conversion;
  they are not a script runtime.
- **Variables and layout:** use single-pass `${name}` interpolation (`$${name}` for a
  literal), then configure pages, grid rectangles, panel type, time window, chart/table/
  text/image options, and closed button actions. Variables are never recursively evaluated.
- **Thresholds and links:** configure panel thresholds and style, alert-rule expression/
  severity/surfaces, and valid `linked_panel_ids` / `style.linked_panel_id`. Chart/table
  bindings require `source_id + metric`; dual/triple cards require independent linked series.
- **Refresh:** each dashboard source and panel has its own schedule. A refresh action only
  polls data; it is not an e-paper full refresh. DeviceSettings `data_refresh_s` overrides
  legacy alert `http_poll` sources only, while `epd_full_refresh_s` is a separate display
  policy. Runtime panel scheduling is at least one minute; avoid animations.

### Secret limitation that must not be skipped

The runtime can resolve `auth_token_ref: "nvs:<key>"` or `nvs:<ns>/<key>` into a Bearer
header. The final baseline has **no public UI/API/serial workflow for writing an arbitrary
source token into NVS**, so the Bearer store-reference-test chain is not complete or
accepted. Do not put secrets in presets, URLs, bundles, mocks, shell history, or logs.
Use ordinary headers only for non-secret values. Sensitive custom headers are masked in
API responses but are currently persisted verbatim to SD `dashboard.json`; therefore
`X-API-Key`, Cookie, and similar headers must not hold secrets. Raw 2xx responses are
also cached, so servers should return only the minimum data required by the panel.

## 3. Input, BLE observer, settings, and recovery

Final defaults: Up/Down short press moves focus and long press changes pages; Power
short activates and long returns; Front short is fixed Home, Front long opens Help,
and Front double opens Status. Up/Down double and chord have no hidden default action.
Horizontal swipes change pages. Custom bindings are limited to 24 sparse overrides and
nine triggers, with exact-page → dashboard-group → global → default precedence. Use
`GET/PUT /api/v1/input-bindings`, `/schema`, and `/reset`; only whitelisted builtins and
registered panel actions are accepted. Power, Front Home, confirmation, and recovery
cannot be remapped.

For `confirm=true` and alert acknowledgement, the first activation only arms the action.
Freshly press and hold the same control for at least 700 ms within five seconds; the
arming press and a second tap do not count. Movement, other input, timeout, or a page/
snapshot change cancels. Network recovery, WiFi reset, factory reset, BLE provisioning,
and BLE unbind require physical Power specifically.

DeviceSettings and `GET/PUT /api/v1/settings` share typed validation, atomic NVS
persistence, and live subscriptions for language, power, frontlight, refresh, sleep,
timezone, diagnostics, and BLE observer. The schema endpoint documents ranges; status
also exposes capabilities, power, and BLE counters. Frontlight is explicitly turned off
before deep sleep and restored from saved settings on wake. SD/RTC/frontlight availability
is reported from actual probing, and unsupported writes fail rather than pretend success.

BLE observer is off by default and passively scans advertisements; it is not persistent
phone GATT. Configure at most four manufacturer-ID or service-UUID rules, each with one
to four bounded numeric fields, optional payload matching, TTL, and deduplication. Scan
windows are 2–60 seconds with a 30–86400 second following interval; battery mode scans
at most once per wake window. Matches produce bounded alert events with numeric fields
and device/RSSI/rule metadata, not arbitrary code or HTTP dashboard metric series.
WiFi/BLE RF coexistence, phone compatibility, packet loss, and power draw still require
physical acceptance. Bluetooth Classic, audio, generic Bluetooth networking, persistent
phone GATT, and remote OTA are outside this release. HTTP and BLE are unreachable during
deep sleep.

Local recovery AP, WiFi-only reset, and factory reset all use physical-Power fresh hold.
WiFi-only reset preserves the admin password; forgetting that password requires factory
reset. Factory reset clears the documented NVS namespaces, DeviceSettings, input bindings,
and listed config/alerts/dashboard files while preserving fonts and unknown user files.
Unavailable SD or incomplete erasure prevents a success reboot.

## 4. Acceptance status

Host/static/mock/native-C++/DOM checks and the full ESP-IDF build pass on the final
baseline, including all 30 presets and four scenarios. They are not physical-device
acceptance. Display/fonts/grayscale, touch and electrical timing, online device HTTP,
BLE/RF coexistence, SD/RTC/frontlight, deep-sleep current/wake behavior, and 72-hour
stability remain explicitly unverified. Use the categorized checklist above and bind
any future physical sign-off to the exact flashed firmware SHA-256.
