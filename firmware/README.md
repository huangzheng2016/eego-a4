# EEGO A4 告警/数据面板固件

EEGO A4（ESP32-S3 + 7.8" UC8279C 墨水屏）的私有告警面板固件：纯 ESP-IDF
（无 Arduino 依赖），WiFi 局域网直连，轮询/推送双路径采集告警，墨水屏展示
总览/列表/详情，串口 diag 通道 + 主机工具完成联调与 smoke 验证。

> 许可：本目录代码为 GPL-3.0-or-later 衍生作品（移植自 eego-a4-weread，GPL-3.0；
> 板级硬件事实来自 eego-a4-template-firmware，MIT）。第三方组件许可见
> [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)，来源与商标边界见
> [NOTICE.md](NOTICE.md)。

## 构建

前提：ESP-IDF v5.5.1（`IDF_PATH` 已配置）。目标芯片 ESP32-S3（16MB Flash +
Octal PSRAM，EEGO A4）。

```sh
cd firmware
idf.py build          # sdkconfig.defaults 自动应用（首次构建时）
idf.py size           # 体积/内存预算核对
```

- 分区表 `partitions.csv`：16MB A/B OTA（app0/app1 各 0x6B0000）+ NVS +
  otadata + spiffs + coredump；已启用 bootloader 回滚保护。
- PSRAM 只经 `heap_caps_malloc(MALLOC_CAP_SPIRAM)` 显式分配；普通 malloc
  不触碰 PSRAM（避免 WiFi/NVS 内部分配到 PSRAM 导致 flash 写崩溃）。
- 静态自检（不依赖 IDF 环境）：

```sh
python3 firmware/tools/validate_firmware.py   # 依赖方向/必需文件/禁止残留/许可头/schema 一致性
```

## 刷写（仅 USB，安全实践）

```sh
idf.py -p /dev/cu.usbmodem* flash monitor     # Linux 为 /dev/ttyACM*
```

安全边界（首版强制）：

- **固件不提供任何远程 OTA/固件上传端点**（`/api/v1/firmware/*` → 404）；
  刷写只走 USB。OTA 分区已预留，未来必须签名校验 + A/B 回滚后才开放。
- 刷写前按仓库 `docs/RECOVERY.md` 工作流备份原固件（esptool read-flash）、
  核对目标分区与镜像哈希；刷写后用 `python3 -m esptool verify-flash` 校验。
- 模板仓库的 web_flasher（esptool-js，Apache-2.0）可复用为 USB 浏览器刷机
  入口；它不在本目录内，使用时遵守其许可。
- 管理 API 状态端点暴露 `fw_version` + `fw_sha256`，刷写后人工核对。

## 首次配网

设备无管理密码时上电进入初始化配网模式：

1. 设备开放 AP「EEGO-A4-Setup」（固定 SSID，不含设备唯一标识）+ APSTA。
2. 手机/电脑连上该 AP，浏览器访问 `http://192.168.4.1/`：
   第一步扫 WiFi 选网络输密码；第二步**强制设置管理密码**（≥8 位，两次一致）。
3. 提交后设备以 APSTA 试连 STA（15s 超时）：成功 → 凭据与管理密码
   （PBKDF2-HMAC-SHA256 哈希）写入 NVS → 关门户 → 重启进入正常模式；
   失败显示中文原因，停留门户可重试。
4. 门户窗口 10 分钟无操作自动关闭（屏幕有提示），之后需本地操作重新进入。

忘记管理密码 = 恢复出厂（屏幕设置页 5 秒内二次确认，或远程
`POST /api/v1/system/factory-reset` 带密码 + `confirm:"RESET"` 双确认），
**不留后门**。

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

不要把管理 token 写进 preset、mock、命令历史或日志。数据源的长期凭据使用现有 NVS
credential 流程，bundle 只保存 `auth_token_ref: "nvs:<key>"`；`Authorization` 请求头由
运行层解析引用后临时生成，不能作为普通 headers 持久化。

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

### 设备交互与刷新语义

- 触控按钮直接命中对应 action；点选 alert 行可聚焦事件并跳到 linked panel；底部左右
  区域切换页面。Up/Down 短按移动焦点，Power 短按激活，Power 长按返回；Up/Down 长按
  在多动作按钮间切换、轮换 chart/变量或调整数据刷新周期。
- action 仅支持 `goto_page/goto_panel/set_variable/refresh/back/ack_alert`。`confirm=true`
  或 ack 需要在 5 秒窗口内再次确认；目标在最新快照上重新校验。`refresh` 的 `all/current/
  <source>` 只安排**数据轮询**，不等于强制墨水屏全刷。
- panel 数据/文本/图片或状态变化按 fingerprint 标脏，未变化可跳过绘制；结构、翻页和
  布局变化会触发更大的重绘。面板周期可覆盖为配置默认或 1/5/15/30 分钟，运行层实际
  panel 调度最小 1 分钟。电子纸不适合秒级动画，建议普通源 1–5 分钟、天气 5–15 分钟、
  图片 15 分钟以上，并按信息价值减少全屏变化。
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

## 供电模式

设置页/串口 `status` 可见当前模式；`PowerMode` 三档（NVS 持久）：

- **Auto（默认）**：按充电状态自动判定——充电中=常供电，否则=电池；无电池
  读数（台式）视为常供电，避免误判进休眠。
- **常供电（Mains）**：modem-sleep 常驻，WiFi 即时在线，http_push 即时到达，
  面板按快照刷新。
- **电池（Battery）**：按周期轮询——唤醒 → 等 WiFi → 全源轮询 → 静默 →
  交互窗口 → 安全休眠（GPIO4 保持锁存）；http_push 延迟到下一周期。

电池信息：电压/百分比/充电状态经 ADC 分压（÷1.559）读取，`battery` 串口
命令与状态页可见。

## 安全边界

- **刷写**：仅 USB；无远程 OTA/上传端点（一切未知路径 404）。
- **secrets**：WiFi 密码/管理密码哈希/ingest key 只存 NVS（`wifi`/`sec`
  命名空间）；`config.json` 只有非机密配置与 `*_ref` 引用；会话令牌 RAM
  only（重启即清，最多 3 个 LRU）；任何 API 响应/串口日志/SD 文件不含秘密
  明文（管理配置输出默认 `********` 掩码）。
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
/sdcard/alertpanel/config.json      # 管理配置（schema_version=1；secrets 只有 *_ref）
/sdcard/alertpanel/dashboard.json   # dashboard bundle + revision（原子持久化）
/sdcard/alertpanel/alerts.db        # 告警库（JSONL，原子持久化）
/sdcard/alertpanel/cache/<src>.json # 告警轮询源 2xx 原始响应缓存
/sdcard/alertpanel/dashboard_cache/ # dashboard 历史、归一化快照和图片缓存
/sdcard/fonts/...                   # 可选 UI 字体（.eefont/.bin，配置 admin.display.font）
```

SD 未挂载时：告警库降级为内存运行（重启丢失），状态页如实显示；dashboard 持久缓存
不可用不能被报告为已保存。缓存和导出仍不得包含凭据明文。

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

## 真机验证清单

上机前：`validate_firmware.py` 全绿 + `idf.py build` 通过。上机后按序核对：

- [ ] 上电启动：串口出现 diag 横幅；`version`/`status` 输出齐全（boot 七项
      latch/touch/buttons/frontlight/rtc/sd/display 符合实机硬件）
- [ ] 显示：全量刷新正常；`smoke` 的四级灰条 **目视** 确认（灰阶 0/4/8/12
      可辨）；中文/英文界面文字渲染正常（SD 字体或内置 5x7 回退）
- [ ] 触控/按键：列表滚动、详情打开、ack/清除操作有反馈（`page` 命令可台架跳转）
- [ ] SD：`sd` 命令 ready=1；`/alertpanel` 目录自动建立；拔插卡状态页如实变化
- [ ] 首次配网：AP「EEGO-A4-Setup」出现 → 门户两步 → 重启后 `net` 显示
      wifi_state=2（Connected）
- [ ] mock 轮询：配 `mock-normal` 源后 `alerts` 出现 mock 告警；`dup` 场景
      occurrences 递增；`clear` 场景缺席后自动清除；`ttl` 场景到期 expired；
      `oversize`/`badlen` 场景 `net` 源健康计数 skipped/fail 递增且不崩溃
- [ ] http_push：`curl -X POST -H "Content-Type: application/json" \
      -H "X-Ingest-Key: <key>" -d '{"type":"home.water_leak","severity":"critical",\
      "title":"t","message":"m"}' http://<ip>/api/v1/alerts` 提交单条/批量，
      面板出现对应告警
- [ ] 确认/清除：面板触控 ack、串口 `ack-all`、API `POST /alerts/{id}/ack`
      三路径状态一致；`alerts` 摘要计数正确
- [ ] API 登录：`smoke_serial_test.py` 的 api 项（登录/负例 401/status/logout）
- [ ] 供电模式：电池档下轮询-休眠-唤醒循环；常电档下 http_push 即时到达
- [ ] 告警持久化：重启后 `alerts.db` 恢复在册告警
- [ ] DP 四场景：TOKEN、天气、服务器、网络安全示例经 HTTP 校验/应用，窗口数据、
      事件、文本与 PNG 均正确；source test 不泄露原始响应或凭据
- [ ] DP 交互/预算：双/三指标 linked cards、按钮组、变量切换、二次确认 ack、局部/
      全量刷新、离线缓存、大图内存水位和电池唤醒窗口逐项核对
- [ ] 连续运行（soak）：≥72h 无异常重启、内部 RAM 水位稳定（P3 项）

仅运行 `validate_firmware.py`、mock `--self-test` 或主机 C++ parser 时，30 个预设的
实际排版/字体/灰阶、触控与按键、图片解码峰值、网络重试、SD 持久化和墨水屏刷新耗时
**均为真机未验证项**；不得把这些主机结果写成整产品真机验收。未真机验证项会在发布
记录中如实标注（构建产物未刷入设备时，结论严格绑定实体机镜像哈希，见仓库
`docs/VALIDATION.md`）。
