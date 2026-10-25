# EEGO A4 固件

基于 ESP-IDF 的可配置监控面板固件，无 Arduino 依赖。设备只有一个主界面：
在 web 管理端配置并应用的 dashboard。HTTP 数据源经声明式解析后驱动 card、
chart、table、text、image、button、alert 七类 panel；告警来自 dashboard 告警
规则和外部推送（`POST /api/v1/alerts`），统一显示在告警面板与底部横幅中。
复杂转换放在用户服务器完成，设备不执行任意脚本。

[预设与完整示例](presets/README.md) · [仓库入口](../README.md)

## 构建与校验

需要 ESP-IDF v5.5.1，目标为 ESP32-S3、16 MiB Flash + Octal PSRAM。
在仓库根目录执行：

```sh
python3 firmware/tools/validate_firmware.py
python3 firmware/tools/preset_tool.py validate-library
idf.py -C firmware -B .tmp/firmware-build build
idf.py -C firmware -B .tmp/firmware-build size
idf.py -C firmware -B .tmp/firmware-build size-components
```

`validate_firmware.py` 检查组件依赖、必需文件、许可、schema 和预设；它不做编译、
刷写或真机验收。分区使用 `firmware/partitions.csv`，刷写时只使用同一构建目录生成
的镜像。预留 A/B 分区不代表已开放 OTA。

## 本机默认凭据（可选）

不想使用配网热点时，可在 `firmware/main/local_secrets.h` 定义本机默认值：

```cpp
#define EEGO_LOCAL_WIFI_SSID "your-ssid"
#define EEGO_LOCAL_WIFI_PASSWORD "your-wifi-password"
#define EEGO_LOCAL_ADMIN_PASSWORD "your-admin-password"
```

该文件被 `.gitignore` 排除。仅当 NVS 中没有 WiFi 账号或管理密码时，固件会在首次
启动写入对应默认值，之后不覆盖修改。需要替换现有 WiFi 账号时，可临时定义
`EEGO_LOCAL_WIFI_REPLACE`，写入新默认值后应移除并重新构建，避免每次启动覆盖。
需要临时关闭深睡时，可定义 `EEGO_LOCAL_POWER_MODE_MAINS`，确认 Mains 生效后移除；
NVS 会保留该电源模式，之后仍可在设置中改回 Auto 或 Battery。新设备默认显示时区为
UTC+8，可在设置中逐小时调整；已有设备可用 `EEGO_LOCAL_TIMEZONE_OFFSET_MIN` 写入一次。

**这样构建的固件和备份都含明文秘密，必须私有保存，不得上传 GitHub、Release 或公开分享。**

## 刷写与首次配置

刷写前按[备份与恢复](../docs/RECOVERY.md)保存整片 Flash。确认端口后执行：

```sh
idf.py -C firmware -B .tmp/firmware-build -p <PORT> flash monitor
```

1. 无管理密码时，设备开放 `EEGO-A4-Setup`；连接后打开 `http://192.168.4.1/`。
2. 选择 WiFi，并设置至少 8 位管理密码。STA 试连成功后才保存并重启。
3. 重启后设备显示「开始使用」引导页，上面就是管理地址 `http://<device-ip>/`
   （也可用串口 `net` 查询）；用同一网络的电脑或手机打开并登录。
4. 在「数据面板」选择模板或导入 JSON，先校验再应用；导入只改草稿，应用后
   设备立即切换到该 dashboard。删除 dashboard 后设备回到引导页。

`http://<device-ip>/` 是一个加载器：从 GitHub Pages 拉取管理 Web 应用
（`EEGO_WEB_APP_BASE`，可在编译时覆盖），应用在设备同源下调用 `/api/v1`。
固件不再内置管理页：浏览器无法访问 GitHub Pages 时，加载页只显示中英文提示
（检查访问设备的电脑/手机能否上网）。配网门户（AP 模式）始终内置、可离线使用。

可直接导入 `presets/examples/*.json`，或展开主机侧预设：

```sh
python3 firmware/tools/preset_tool.py list
python3 firmware/tools/preset_tool.py export chart-kpi \
  --mock-host 192.168.1.20:8978 --output .tmp/chart-kpi.json
python3 firmware/tools/preset_tool.py validate .tmp/chart-kpi.json
```

`eego.dashboard-preset/1` 不能原样导入；export 后才是 `eego.dashboard-bundle/1`。
同一套预设与示例也内置在固件中，可通过 `GET /api/v1/dashboard/templates` 获取；
修改 `presets/` 后运行 `python3 firmware/tools/preset_tool.py embed` 重新生成
`components/admin/src/dashboard_templates.inc`（`validate-library` 会检查是否过期）。
真机联调时 `--mock-host` 必须是电脑的局域网地址，不是 `127.0.0.1`。

## 设备操作

屏幕：

| 屏幕 | 进入方式 | 内容 |
|---|---|---|
| 主页 · Dashboard | 开机、前置键短按 | 已应用的 dashboard（多页/多子屏），底部状态行显示屏号、时钟来源、数据源在线数与最新告警横幅 |
| 主页 · 开始使用 | 未配置 dashboard 时 | 管理地址（或配网热点与 `http://192.168.4.1/`）、WiFi 状态、三步配置说明 |
| 告警详情 | 点按告警面板中的一行或底部横幅；告警面板上按 Power | 严重度、状态、来源、时间与字段；↑/↓ 浏览同一面板的告警；确认 / 静音 / 删除 / 趋势 |
| 设备状态 | 前置键双击；引导页软键 | WiFi、信号、地址、时间来源、电池与供电、存储（alerts.db 大小）、硬件、固件版本、运行时长、各数据源最近更新与错误、外部告警计数 |
| 设置 | 前置键长按；引导页软键 | 本机设置（网络恢复、前光、刷新周期、休眠、静音列表、按键映射重置等） |
| 输入帮助 | 设置 → 输入帮助 | 本节按键说明 |

按键与手势（↑/↓ 与前置键长按/双击可在 web 端自定义，其余固定）：

| 输入 | 动作 |
|---|---|
| ↑ / ↓ 短按 | 上一屏 / 下一屏（dashboard 子屏优先、跨页循环）；设置、告警详情中为上/下一项 |
| ↑ / ↓ 长按 | dashboard：显示并移动面板焦点框；设置：上/下一子页 |
| Power 短按 | 选择/确认：告警面板 → 告警详情；按钮面板 → 执行动作；本机页面 → 当前软键/行。未选面板时先打开本屏告警，否则显示焦点框 |
| Power 长按 | 返回：子页面回主页；dashboard 先退跳转栈，再回默认页第一屏 |
| 前置键 短按 / 长按 / 双击 | 主页 / 设置 / 设备状态 |
| 左滑或上滑 / 右滑或下滑 | 下一屏（项）/ 上一屏（项）；子页面首屏右滑 = 返回 |
| 点按 | 告警行 → 详情；按钮 → 动作；其余面板 → 选中；底部状态行左/右半 → 上/下一屏 |

告警详情中「确认」对外部告警写入设备告警库（等同 `POST /alerts/{id}/ack`），对
规则告警只在本机视图中压制本次触发；「静音」「删除」只对外部告警可用，静音按指纹
持久化到 `config.json` 的 `mute_rules`（设置 → 静音列表 可清空）。

## 数据面板

- 数据源支持标准 JSON、声明式 JSONPath、Prometheus、文本和 PNG/JPEG。
- metric 使用 `metric/value/unit/timestamp_ms/labels`；事件使用
  `id/rule_id/timestamp_ms/severity/state/title/message/linked_panel_id`。
- 页面使用 grid 布局；可配置变量、时间窗口、阈值、告警规则、linked panel 和
  source/panel 独立调度。表达式和按钮 action 都是白名单，不是脚本。
- 布局规则（设备、固件校验、主机校验共用）：`grid_columns` 按比例映射到屏幕
  宽度（任意列数和边界都可）；每一屏固定 8 个 grid 行，按比例映射到屏幕高度；
  可见 panel 不得重叠，且必须完整落在一屏内（`h ≤ 8` 且 `y % 8 + h ≤ 8`），
  第 n 屏对应 `y` 在 `[8n, 8n+8)`。页面没有 `row_height` 字段（未知字段一律拒绝）。
- 只接受当前格式：dashboard 必须是 `"schema":"eego.dashboard/2","schema_version":2`，
  bundle 必须是 `eego.dashboard-bundle/1`。不做任何旧格式迁移或布局修复；SD 上
  不合规的 `dashboard.json`/`config.json` 会被改名为 `.corrupt`，设备以未配置/默认值启动。
- SD 上的文件替换（`config.json`、`dashboard.json`、`alerts.db`、数据源缓存）统一走
  `eego_hal/atomic_file.h`：写 `.tmp` 并 fsync → 目标改名 `.bak` → `.tmp` 改名为目标 →
  删 `.bak`（FatFS 的 rename 不能覆盖已存在文件）。中途断电只剩 `.bak` 时，启动/读取
  前自动恢复。
- dashboard source 与对应 source plan 的连接字段必须一致；Web 表单会自动同步，
  手工编辑 JSON 时需自行保持 `id/kind/url/refresh_ms/timeout_ms`、TLS、headers
  和凭据引用一致。
- 多客户端写入携带 `If-Match: <revision>`，过期 revision 返回 409。
- 电子纸不适合秒级动画；panel 调度最小 1 分钟，数据刷新不等于墨水屏全刷。
  字段和资源上限以 `GET /api/v1/dashboard/schema` 及主机 schema 校验为准。

管理 API 均使用 Bearer 登录，除登录和告警提交外：

| 端点 | 用途 |
|---|---|
| `POST /api/v1/auth/login` | `{"password":"..."}` 获取会话令牌 |
| `GET /api/v1/status` | 设备、网络、存储和告警状态 |
| `GET/PUT /api/v1/config` | 非秘密管理配置：`admin`、`alert_type_overrides`、`mute_rules`（`admin.display` 只含 `font`；前光亮度/色温只在 `/settings`；数据源只在 dashboard bundle） |
| `GET /api/v1/alerts`、`GET /api/v1/alerts/{id}` | 外部告警库（推送摄入的记录） |
| `POST /api/v1/alerts/{id}/ack`、`/clear`、`/alerts/ack-all` | 外部告警确认/清除 |
| `GET/PUT/DELETE /api/v1/dashboard` | 读取、校验后替换或删除 bundle |
| `POST /api/v1/dashboard/validate` | 只校验候选 bundle，不应用；返回 `{"valid","errors":[{"field","message"}],"bundle"?}` |
| `POST /api/v1/dashboard/sources/test` | 测试草稿数据源，返回归一化预览 |
| `POST /api/v1/dashboard/sources/probe` | 抓取一次原始响应（敏感键值打码、≤16 KiB），用于挑选 JSONPath |
| `GET /api/v1/dashboard/templates` | 内置预设库与完整示例 |
| `GET /api/v1/display/snapshot.bmp` | 当前屏幕帧（552×768 1-bpp BMP）；未接线时 503 |
| `POST /api/v1/dashboard/render-preview` | 设备端离屏渲染候选 bundle 的一屏，返回 552×768 4-bpp BMP（见下） |
| `GET/PUT /api/v1/settings` | 本机设置；schema 在同路径加 `/schema` |
| `GET/PUT/DELETE /api/v1/sources/credentials` | 管理数据源凭据 |
| `GET/PUT /api/v1/input-bindings` | 自定义输入；另有 `/schema` 和 `/reset` |
| `POST /api/v1/system/reboot` | 重启，需 `confirm:"REBOOT"` |

`POST /api/v1/dashboard/render-preview`（Bearer，`Content-Type: application/json`，
每会话 10 次/分钟，同一时刻只渲染一个）：

- 请求：`{"bundle": <bundle>, "page_id"?: "<page>", "screen"?: <int ≥0>, "refresh"?: "full"|"fast"}`。
  `bundle` 与 `/dashboard/validate` 走同一校验；`page_id` 缺省 = `default_page_id`
  （否则第一个可见页）；`screen` 缺省 0；`refresh` 缺省 `full`。整个请求体 ≤192 KiB。
- 成功：`200 image/bmp`，552×768、自上而下、4-bpp、4 色灰阶调色板
  （索引 0/1/2/3 = `#000`/`#555`/`#AAA`/`#FFF`，即 Full 刷新显示的四级灰阶；
  `refresh:"fast"` 时按快刷阈值只输出黑/白）。响应头 `X-Preview-Page`、
  `X-Preview-Screen`、`X-Preview-Screens`、`X-Preview-Data`
  （`bundle`/`live`/`mixed`/`placeholder`/`none`）。
- 数值来源：bundle 自带的 `samples`/`events`；bundle 没有样本的数据源用设备上同 id
  数据源的实时数据；仍完全没有样本时，每个指标面板用与 Web 编辑器相同的确定性演示序列。
  远程图片不抓取（显示占位），SD 本地图片照常渲染。底部状态行显示 `Preview <页> n/m`。
- 错误：`400 invalid_request`（请求字段不对）、`400 validation_failed`
  （bundle 校验失败，`error.fields` 为逐字段错误；或页/屏不存在）、
  `415`（非 JSON）、`413`（过大）、`429`（限速，带 `Retry-After`）、
  `503 unavailable`（另一个预览正在渲染、UI 任务 15 s 内未处理、PSRAM 不足或未接线）。
- 渲染在 app_task（UI 属主）经管理命令队列执行，使用独立的 PSRAM 离屏帧缓冲，
  不影响屏幕内容；渲染期间设备输入短暂排队。

`POST /api/v1/alerts` 使用独立的 `X-Ingest-Key` 提交规范事件，支持单条和批量；
ingest key 不能读取管理资源。具体字段以设备 schema 和校验错误为准。推送的告警
进入设备告警库（确认/静音/过期/持久化），并作为 dashboard 事件出现在告警面板
（`source_id` 为空 = 全部告警，或为 `http_push` = 只看推送告警）和横幅中；没有告警面板的 dashboard
只在横幅显示。设备不再轮询「告警源」：拉取型数据一律配置为 dashboard 数据源，
再用告警规则产生告警。

## 数据源凭据

推荐顺序：**写入凭据 → 保存引用 → 测试并应用**。

在「数据面板 → 凭据」操作，或调用：

```text
GET    /api/v1/sources/credentials
PUT    /api/v1/sources/credentials   {"key":"weather","value":"<secret>"}
DELETE /api/v1/sources/credentials   {"key":"weather"}
```

秘密只写 NVS `source` 命名空间，API 不提供读回。Bearer 使用
`"auth_token_ref":"nvs:source/weather"`；其他秘密头使用
`"header_secret_refs":{"X-Credential":"nvs:source/api"}`。普通 `headers`
只能保存非秘密值，不能保存 Authorization、Cookie、token 等字面值。

同 key PUT 即轮换；仍被 dashboard sources 或 plans 引用时不能删除。
缺失或非法引用会使请求失败，不会降级成无认证请求。原始 2xx 响应会按设计缓存，
服务器只应返回面板需要的最小数据，配置存引用不等于缓存中没有秘密。

## 输入、BLE 与电源

- 按键与手势见「设备操作」。自定义绑定最多 24 条，页面上下文为
  `dashboard`、`dashboard:<page_id>`、`onboarding`、`detail`、`status`、`settings`、
  `help` 或 `*`；动作只能是白名单 builtin（`page_previous/page_next/focus_previous/
  focus_next/home/settings/status/help/refresh`）或已注册 panel action。
  Power、前置键短按（主页）、确认和恢复流程不可重映射。
- 危险操作第一次激活只布防；5 秒内必须从同一控件重新按住至少 700 ms。
  网络恢复、工厂恢复和 BLE 配网/解绑只接受实体 Power。
- 首次管理密码必须在 AP 门户设置。之后的本地 BLE 配网使用限时窗口、随机 PoP
  和 Security 1；被动 BLE observer 默认关闭。Bluetooth Classic、持续手机 GATT
  和通用蓝牙上网不在范围内。
- Auto 按充电状态选择常电或电池模式。电池模式只在唤醒窗口联网；深睡时 HTTP
  和 BLE 不可达，HTTP 推送会延迟到唤醒后。

忘记管理密码只能全量恢复出厂。WiFi-only 重置不会清除管理密码或数据源凭据。
危险恢复操作可从本机设置页发起，并需实体 Power 确认。

## SD 与诊断

```text
/sdcard/alertpanel/config.json       管理配置与 NVS 引用
/sdcard/alertpanel/dashboard.json    当前 bundle 与 revision
/sdcard/alertpanel/alerts.db         外部告警记录（JSONL；启动时与超过 256 KiB/死行过半时压缩）
/sdcard/alertpanel/dashboard_cache/  面板历史、快照和图片缓存
/sdcard/fonts/                       可选 UI 字体
```

中文显示可将自备的 Noto Sans CJK 小尺寸字体放到
`firmware/components/eego_ui/fonts/NotoSansCJKsc_6.cpfont` 后重新构建；该文件被
`.gitignore` 排除，会在构建时嵌入固件，无需取出 SD。没有嵌入文件时仍按 SD 根目录
字体或内置 ASCII 回退处理。字体由使用者自备并遵守其许可（Noto 为 SIL OFL 1.1）。

SD 不可用时告警降级为内存运行，重启后丢失；不能把这种状态报告为已持久化。

常用主机工具：

```sh
python3 firmware/tools/mock_dashboard_server.py --host 0.0.0.0 --port 8978
python3 firmware/tools/smoke_serial_test.py --port <PORT> \
  --admin-password '<管理密码>'
```

mock 不需要真实凭据；串口 smoke 只测试，不刷写设备。

## 当前边界

- 仅 USB 刷写，无远程 OTA；局域网管理使用明文 HTTP，只在可信网络操作。
- 未启用 flash encryption / secure boot v2；逻辑删除不保证擦除备份、旧 Flash 页
  或 coredump 中的历史秘密。
- 静态、mock、主机同源 C++、DOM 和构建检查不等于真机验收。显示、触控、电气、
  真实网络、BLE/RF 共存、功耗和长期稳定性仍需在实体设备上验证。

许可为 [GPL-3.0-or-later](LICENSE)；来源见 [NOTICE.md](NOTICE.md)，第三方组件见
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
