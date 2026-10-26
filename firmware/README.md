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

不想使用配网热点时，可复制模板 `firmware/main/local_secrets.example.h` 为
`firmware/main/local_secrets.h` 并填写本机默认值（模板逐项说明了全部宏）：

```cpp
// 按顺序尝试，列表顺序即优先级；最多 8 个，密码为空表示开放网络
#define EEGO_LOCAL_WIFI_LIST { {"home-ssid", "home-password"}, {"office-ssid", "office-password"} }
#define EEGO_LOCAL_ADMIN_PASSWORD "your-admin-password"   // 8..128 字节
```

只有一个网络时也可用简写 `EEGO_LOCAL_WIFI_SSID` / `EEGO_LOCAL_WIFI_PASSWORD`
（同时定义时以 `EEGO_LOCAL_WIFI_LIST` 为准）。SSID 超长/重复、WiFi 密码不是空或
8..63 字节、管理密码短于 8 字节时会在编译期报错。

该文件被 `.gitignore` 排除。每次启动都会确保默认 WiFi 列表生效：列表中的网络一定
存在、密码以编译期为准、按列表顺序排在最前；之前保存的其他网络（配网门户、管理端或
旧固件写入）保持原顺序排在后面，超过 8 个时丢弃末尾的（启动日志打印最终 SSID 列表）。
管理密码仅在尚未设置时写入。定义 `EEGO_LOCAL_WIFI_REPLACE` 时只保留默认网络。

联网时固件先扫描，可见的已保存网络按列表顺序优先尝试（各两次，全信道扫描后按信号选
同名 AP），未扫描到的网络（隐藏 SSID 等）随后各试一次；全部失败后指数退避（10s 起，
封顶 60s）再遍历整表，断线后立即重新遍历。开放网络（空密码）按 OPEN 阈值连接。国家码
默认 CN（信道 1–13，可用 `-DEEGO_WIFI_COUNTRY_CODE='"XX"'` 覆盖）。连续 3 轮失败
（或已存网络 2 分钟都扫不到）才自动打开恢复热点，热点信道取信号最强的已存网络所在
信道；热点开着时 STA 继续重试，连上后自动关闭热点（有客户端连着时宽限 2 分钟）。
web 管理端「网络 / WiFi」页可增删网络、改密码、拖动排序并查看当前连接。

### WiFi 离线诊断

不需要互联网：手机连上设备热点 `EEGO-A4-Setup` 后打开 `http://192.168.4.1/wifi-diag`。
页面列出已保存网络（按优先级）、每个网络最近一次尝试时间/结果、最近与本次开机首次的
断开原因码及中文说明、最近一次扫描（SSID、信号、信道、加密方式，已保存的网络标★），
并可「立即重试」「重新扫描」。JSON 在 `GET /api/v1/wifi/diag`（`POST /api/v1/wifi/retry`、
`POST /api/v1/wifi/scan`）。已设管理密码时（含恢复热点）数据端点需要 Bearer 登录，页面
内输入管理密码即可；首次配网门户期间（尚无管理密码）与门户其余端点一样免认证。输出
只含 SSID/信号/原因码，绝不含密码。设备「状态」页网络分节与串口 `wifi`（`wifi scan`、
`wifi retry`）显示同样的信息。

### WiFi 信号优化

信号分档：≥ -60 dBm 很好、-60～-70 良好、-70～-80 一般、< -80 很弱。状态页、
`/api/v1/status`、`/wifi-diag` 与 web「WiFi」页都会显示分档、近 10 分钟最低/平均信号、
beacon 丢失与漫游次数。低于 -80 dBm 时常见丢包、延迟高、DHCP 很慢，建议：

- **摆放**：把设备的天线一侧（ESP32-S3 模组所在边）朝向路由器；远离金属外壳、金属
  书架、显示器背板和微波炉；不要平放在金属桌面上。
- **路由器**：2.4 GHz 固定信道 1、6 或 11（关闭"自动信道"，避免路由器跳信道后重新扫描）；
  频宽设为 20 MHz（不要 20/40 自动或 40 MHz）；不要启用"仅 802.11ax（WiFi 6）"模式，
  保持 b/g/n/ax 混合；2.4G 与 5G 同名时设备只用 2.4G。
- **mesh / 多 AP**：在设备附近放一个 mesh 节点或同名 AP。固件开启 802.11k/v，并在信号
  持续低于 -80 dBm 超过 60 秒、且同名的另一台 AP 强至少 8 dB 时自动切换（最多每 5 分钟一次）。

固件侧已做：始终 HT20；常供电或保持唤醒时不开 modem 省电（只在电池交互窗口开）；
beacon 丢失容忍 15 秒，单次 beacon 超时先按原 AP 快速重连；已关联但 DHCP 慢时最多等到
关联后 30 秒；墨水屏刷新期间推迟扫描/连接；启动和连上后都会打印实际发射功率（日志里的 `max tx power`）。
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
3. 重启后设备显示「仪表盘 · 未配置」引导页，上面就是管理地址 `http://<device-ip>/`
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
| 主页 · Dashboard | 开机、前置键短按（返回） | 已应用的 dashboard（1–8 屏，每个 page 一屏），底部状态行（正文字号）显示设备状态、「更新于 HH:MM」、屏号 `n/N` 与两端 ‹ › 翻屏键、离线源数与最新告警横幅（见下文「设备状态一眼可见」） |
| 主页 · 仪表盘 · 未配置 | 未配置 dashboard 时（前置键同样回到这里） | 空仪表盘：管理地址（或配网热点与 `http://192.168.4.1/`）、WiFi 状态、三步配置说明 |
| 告警详情 | 点按告警面板中的一行或底部横幅 | 严重度、状态、来源、时间与字段；↑/↓ 浏览同一面板的告警；确认 / 静音 / 删除 / 趋势 |
| 设备状态 | 前置键双击；引导页软键 | 「电源 / 休眠」（正文字号：模式、供电来源与判定原因、保持唤醒剩余、下次唤醒/刷新、唤醒原因、休眠次数）、WiFi、信号、地址、时间来源、电池、存储（alerts.db 大小）、硬件、固件版本、运行时长、各数据源最近更新与错误、外部告警计数；软键 刷新 / 保持唤醒 / 返回 |
| 设置 | 前置键长按；引导页软键 | 本机设置（网络恢复、前光、刷新周期、电池模式与夜间暂停、静音列表、按键映射重置等） |
| 输入帮助 | 设置 → 输入帮助 | 本节按键说明 |

按键与手势（↑/↓ 与前置键长按/双击可在 web 端自定义，其余固定）：

| 输入 | 动作 |
|---|---|
| ↑ / ↓ 短按 | 上一屏 / 下一屏（dashboard 循环翻屏）；设置、告警详情中为上/下一项 |
| ↑ / ↓ 长按 | dashboard：显示并移动面板焦点框；设置：上/下一子页 |
| Power 短按 | 休眠：墨水屏保留当前画面，状态行显示「休眠中 · 下次 HH:MM · 按键唤醒」，按任意实体键唤醒。用电池时与自动深睡相同（定时唤醒取数）；接着外接电源也照样深睡，定时唤醒取数刷屏后继续睡，直到按键。设置页内例外：Power 短按 = 选择当前行（触控失准时仍可进入触摸校准）。危险操作布防期间 = 确认 |
| Power 长按 | 关机：布防后 5 秒内再按一次 Power 确认；屏幕画「已关机 · 按电源键开机」并松开 GPIO4 断电。接着 USB 时板子仍有电，退化为只认电源键的深睡，拔掉 USB 即断电 |
| 前置键 短按 / 长按 / 双击 | 返回（子页面回来源页/主页；dashboard 先退跳转栈，再回第 1 屏）/ 设置 / 设备状态 |
| ↑ + ↓ 同按 | 保持唤醒（默认绑定 builtin `keep_awake`；再按：剩余不足一半时延长，否则取消） |
| 左滑或上滑 / 右滑或下滑 | 下一屏（项）/ 上一屏（项）；子页面首屏右滑 = 返回 |
| 点按 | 选择：告警行 → 详情；按钮 → 动作；其余面板 → 选中；底部状态行左/右三分之一（各 165 px）→ 上/下一屏，中间 → 告警横幅详情 |

告警详情中「确认」对外部告警写入设备告警库（等同 `POST /alerts/{id}/ack`），对
规则告警只在本机视图中压制本次触发；「静音」「删除」只对外部告警可用，静音按指纹
持久化到 `config.json` 的 `mute_rules`（设置 → 静音列表 可清空）。

### 设备状态一眼可见

顶部状态栏（本机页面）与 dashboard 底部状态行（40 px，UI 字体 2x + 同尺寸图标）都显示：

| 图标 | 状态 | 含义 |
|---|---|---|
| ● | 运行中 | 外接电源、最近有交互：屏幕即时更新 |
| ☀ | 保持唤醒 · 剩余 NN 分 | 配置模式：不进节电节奏、不自动深睡（> 10 分钟时按 5 分钟取整，最后 10 分钟逐分钟） |
| 时钟 | 节电 · 下次 HH:MM | 节电刷新节奏（自动进入，只少刷屏；WiFi、取数和网页照常，不等于休眠）：非交互更新合并到该墙钟边界（进入节电时即时刷一次） |
| ◯ | 已唤醒 · 无操作 N 秒后休眠 | 用电池、按键唤醒后的交互期 |
| 月牙 | 休眠中 · 下次 HH:MM · 按键唤醒 / 夜间暂停 · HH:MM 恢复 | 深睡前的最后一帧（数据帧保留），「下次」= 保证刷新的最迟时刻 |

顶部状态栏右侧只画电池图标 + 电量 %（不写供电文字）：外接电源时电池左侧加一个小插头，
电池内的闪电只在正在充电时出现。

底部状态行右侧常驻「更新于 HH:MM」（该帧实际刷屏时刻；深睡帧另附电量），屏幕是否
陈旧一目了然。空间不足时依次省略：页码 → 电量 → 状态标签的后半段。底部状态行高度
与 web 编辑器（`web/src/model/grid.ts` `CONTENT.h = 672`、`STATUS_LINE_Y = 700`）一致。

输入后的「处理中」提示未实现：UC8279C 驱动没有局部窗口刷新，任何提示都要一次整屏
Fast 刷新（并计入 Fast→Full 自动升级计数），反而拖慢真正的更新。

### 保持唤醒（配置模式）

限时覆盖：期间不进入节电刷新节奏、电池交互期不因超时深睡、常供电/电池都不进入夜间
暂停深睡，WiFi 与管理 API 保持在线。时长 `keep_awake_min`（10/30/60/120，默认 30）。
只保存在 RAM，重启或深睡即失效，到期自动结束；到期后按最后一次交互重新计时（不额外
宽限）。触发：设置第一行「保持唤醒 30 分钟」、设备状态页软键「保持唤醒」、↑+↓ 同按
（以上三者：未开启 → 开启；剩余不足一半 → 延长回整段；否则取消），或 web 端：

```text
POST /api/v1/device/keep-awake  {"minutes": 30}   # 0 = 取消，上限 240
→ {"active":true,"remaining_s":1800,"until":<unix>,"default_min":30}
GET  /api/v1/device/keep-awake
```

`GET /api/v1/status` 增加 `keep_awake`（同上）与 `power`：`source`（mains/battery）、
`source_reason`、`power_mode`、`mode`（active/keep_awake/idle/awake/sleeping）、
`battery_pct`/`battery_mv`/`charging`/`usb_host`、`next_wake`、`next_refresh`、
`sleep_in_s`（电池交互期距自动深睡的秒数，-1 = 不会）、`last_wake_reason`、
`sleeps_since_boot`、`last_screen_update`。`GET /status` 与 `GET /device/keep-awake`
是被动轮询，不计入「有人在用」；其余已认证请求照旧推迟电池交互期深睡。

### 切换当前屏

```text
POST /api/v1/device/screen  {"index": 2}     # 第 3 屏（0 起）
POST /api/v1/device/screen  {"step": 1}      # 下一屏（-1 上一屏，循环）
→ {"index":2,"count":5}
GET  /api/v1/device/screen
→ {"available":true,"index":2,"count":5,"page_id":"net","shown":true,"rotate_min":0}
```

Bearer；切屏视为一次交互（立即显示，设备在设置等页面时回到 dashboard）。越界 `400`，
未加载 dashboard `409`。`GET /api/v1/status` 增加同结构的 `screen`。

## 数据面板

- 数据源支持标准 JSON、声明式 JSONPath、Prometheus、文本和 PNG/JPEG。
- metric 使用 `metric/value/unit/timestamp_ms/labels`；事件使用
  `id/rule_id/timestamp_ms/severity/state/title/message/linked_panel_id`。
- 页面使用 grid 布局；可配置变量、时间窗口、阈值、告警规则、linked panel 和
  source/panel 独立调度。表达式和按钮 action 都是白名单，不是脚本。
- 布局规则（设备、固件校验、主机校验共用）：dashboard 有 1–8 个 page，**每个 page
  就是一屏**（可见 page 按顺序为第 1…N 屏）；`grid_columns` 按比例映射到屏幕宽度
  （任意列数和边界都可）；每屏固定 8 个 grid 行，按比例映射到屏幕高度；panel 必须
  满足 `y + h ≤ 8`，可见 panel 不得重叠。页面没有 `row_height` 字段（未知字段一律拒绝）。
- 自动轮播：设置 `screen_rotate_min`（0 = 关，默认；1/2/5/10/30）。无操作满一个间隔后
  按本地墙钟整倍数切到下一屏；节电时随节电刷新节奏显示（上一屏未显示前不前进）；
  电池模式每次定时唤醒至多前进一屏（位置记在 RTC 内存）。
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
  `bundle` 与 `/dashboard/validate` 走同一校验；`page_id` 与 `screen`（第几个可见页，0 起）
  二选一，都缺省 = `default_page_id`（否则第一个可见页）；`refresh` 缺省 `full`。整个请求体 ≤192 KiB。
- 成功：`200 image/bmp`，552×768、自上而下、4-bpp、4 色灰阶调色板
  （索引 0/1/2/3 = `#000`/`#555`/`#AAA`/`#FFF`，即 Full 刷新显示的四级灰阶；
  `refresh:"fast"` 时按快刷阈值只输出黑/白）。响应头 `X-Preview-Page`、
  `X-Preview-Screen`（该页是第几屏，0 起）、`X-Preview-Screens`（可见页数）、`X-Preview-Data`
  （`bundle`/`live`/`mixed`/`placeholder`/`none`）。
- 数值来源：bundle 自带的 `samples`/`events`；bundle 没有样本的数据源用设备上同 id
  数据源的实时数据；仍完全没有样本时，每个指标面板用与 Web 编辑器相同的确定性演示序列。
  远程图片不抓取（显示占位），SD 本地图片照常渲染。底部状态行显示 `预览 · <页>  n/m`。
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
  focus_next/home/settings/status/help/refresh/keep_awake`）或已注册 panel action。
  Power、前置键短按（返回）、确认和恢复流程不可重映射。
- 危险操作第一次激活只布防；5 秒内在同一控件上再按一次（全新短按）确认，长按或其他输入取消。
  网络恢复、工厂恢复和 BLE 配网/解绑只接受实体 Power。
- 首次管理密码必须在 AP 门户设置。之后的本地 BLE 配网使用限时窗口、随机 PoP
  和 Security 1；被动 BLE observer 默认关闭。Bluetooth Classic、持续手机 GATT
  和通用蓝牙上网不在范围内。
- 充电信号极性（设置 `charge_signal`：auto/high/low，设备 设置 → 充电信号）：GPIO11 的有效
  电平默认按 LM3630A 是否在位推断（在位 → 高有效），个别批次相反——表现为**插着充电器
  显示电池、用电池时显示充电中**（且电池时不会深睡）。遇到这种情况把它改成另一个值，
  立即生效，状态栏与供电判定随之更正。
- 供电判定（`main/battery_policy` `MainsDetector`，主机单测 `power_source`）：板上没有
  VBUS 检测脚，只有两条证据——GPIO11 **充电状态**（只在正在充电时有效，充满或拔掉都无效）
  与 USB-Serial-JTAG 的 **USB 主机**连接（接电脑才有，纯充电头没有）。「自动判断」下：
  充电中或连着电脑 → 外接电源；**从电脑上拔下且未在充电 → 当次采样即判为电池**。
  只有最后一条证据是「充电中」（充电头）时才锁存：停充时电压 ≥ 4150 mV 则保持外接电源
  （`full_hold`，此后电压每 10 s 采一次），直到不充电且出现以下任一：中值滤波电压
  < 4120 mV（`below_hold`）、3 分钟内持续下降 ≥ 25 mV（`voltage_drop`，停充后前 2 分钟
  的回落不计）、超过 20 分钟没有再看到充电（充电头满电时会周期性补电，补电重新置位）。
  开机不凭电压锁存，只有开机时看到充电才可能锁存。**残余滞后：满电设备从充电头上拔下，
  要等电压在负载下下降（通常 2–3 分钟，负载很轻时最长 20 分钟）才切到电池；反过来，
  插着充电头但长时间不补电也可能被判成电池。需要立即确定时，把 设置 → 供电模式 改为
  「始终电池」或「始终外接电源」。** 判定结果显示在设备状态页「供电」行（外接电源（USB 已
  连接 / 充电中 / 满电）或 电池，强制时附「手动设定」）、`GET /api/v1/status` 的 `power.source/
  source_reason` 与串口日志 `power mode -> battery (reason: …)`。
  电池模式只在唤醒窗口联网；深睡时 HTTP 和 BLE 不可达，HTTP 推送会延迟到唤醒后。
  行为见下节「电池模式」。

忘记管理密码只能全量恢复出厂。WiFi-only 重置不会清除管理密码或数据源凭据。
危险恢复操作可从本机设置页发起，并需实体 Power 确认。

## 电池模式

电池供电时设备绝大部分时间处于深睡，屏幕（双稳态墨水屏）一直显示最后一帧数据，
不再显示“休眠中”页面（未配置 dashboard 时保留「仪表盘 · 未配置」页，地址框换成「休眠中 · 按任意键唤醒」）。
只有 Power 长按确认的关机才画「已关机」白页。逻辑在 `main/battery_policy.*`（纯逻辑，主机单测
`battery_policy`）与 `main/battery_mode.*`（RTC 内存、SD 帧文件、唤醒源）。

- **对齐唤醒**：每 `battery_refresh_min`（5/10/15/30/60，默认 15）分钟在本地墙钟
  整倍数时刻（如 :00/:15/:30/:45，按设备时区、自本地零点对齐）定时唤醒。定时器提前
  90 s 内醒来记为该时刻，不会为几十秒再醒一次。电量 <20% 间隔翻倍；连续取数失败
  （WiFi 不通或全部源失败）按 ×2/×4/×8 退避，封顶 4 小时；电量 <5% 画一帧“电量过低”
  后只留电源键唤醒。
- **快速路径（定时唤醒）**：跳过触控固件上传、开机白屏、BLE、管理 API、串口诊断和
  alerts.db 启动压缩；墨水屏控制器推迟到确需刷新时才上电。WiFi 用 RTC 内存里的
  上次 BSSID + 信道快连（失败回退扫描）。全部到期数据源一次批量拉取，自启动起 20 s
  硬预算，到点取消在途请求、用已有数据渲染并深睡。期间若接上电源或有人按键，
  以完整应用重启（面板不清屏）。
- **不变不刷**：深睡帧先离屏渲染，对 1-bit 帧与灰阶平面求哈希；与上次显示的帧相同且
  未超过 `battery_max_stale_min`（15–480，默认 60，须 ≥ 唤醒间隔）就完全不碰屏。
  深睡帧状态行显示 `☾ 休眠中 · 下次 15:00 · 按键唤醒 … 75% · 更新于 14:00`（放不下时
  依次省略电量与「按键唤醒」）：电量按 5% 分档并带滞回，“更新于”是上次真正刷屏的
  时刻，“下次”是由它和 max-stale 推出的最迟刷新时刻（而不是下一次定时唤醒——那会让
  每次唤醒的帧都不同、逼出刷屏），因此数据不变时帧逐像素不变，时钟本身不会逼出刷新。
- **跨深睡 Fast 刷新**：每次刷屏后把 B/W 帧 PackBits 压缩、经 atomic_file 原子写到
  `/sdcard/alertpanel/battery_frame.bin`；下次唤醒读回（CRC + 帧哈希校验）预载到
  控制器 DTM1 作为旧帧，再 Fast 刷新。每 12 次刷屏或距上次 Full 满 24 小时、或帧文件
  缺失/损坏时用 Full。
- **夜间暂停**：`sleep_start_min`–`sleep_end_min` 窗口内不唤醒、不联网；进入前状态行
  一次性显示 `夜间暂停 · 06:00 恢复 · 按键唤醒`，睡到窗口结束。常供电模式同样遵守该
  窗口；保持唤醒期间推迟。
- **按键唤醒**：电源键（GPIO8，高有效）为 EXT0；↑/↓（GPIO5/7，RTC GPIO，低有效）为
  EXT1 任一为低唤醒，深睡期间保持 RTC 上拉。触控 GSLX680 没有 INT 引脚（I²C 轮询），
  不能唤醒。按键唤醒运行完整应用（WiFi、管理 API、BLE 扫描窗口），唤醒那一下不当作
  操作；`battery_interactive_timeout_s`（20–600，默认 60）内没有按键/触控/已认证的
  管理请求（被动的 `GET /status` 轮询不算）且未开启保持唤醒，则回到主页渲染深睡帧，
  并深睡到下一个对齐时刻。

能耗依据：一次唤醒中 WiFi（关联 + DHCP + TLS，约 100 mA 量级持续数秒）远大于墨水屏
刷新（Full 约 2–3 s 高压，Fast 不到 1 s），二者都远大于深睡电流。因此先压缩 WiFi
在线时间（快连、批量取数、硬预算、跳过 BLE/httpd），再减少刷屏次数与时长（不变不刷、
跨深睡 Fast、控制器延迟上电）；对齐唤醒让多台设备与数据源的更新节奏可预期。

`diag battery` 打印 RTC 状态（唤醒次数、失败退避、上次刷屏/Full、快连缓存）。真机仍需
验证：深睡电流、单次唤醒能量与时长、DTM1 预载后 Fast 的残影、RTC 慢时钟漂移。

## SD 与诊断

```text
/sdcard/alertpanel/config.json       管理配置与 NVS 引用
/sdcard/alertpanel/dashboard.json    当前 bundle 与 revision
/sdcard/alertpanel/alerts.db         外部告警记录（JSONL；启动时与超过 256 KiB/死行过半时压缩）
/sdcard/alertpanel/dashboard_cache/  面板历史、快照和图片缓存
/sdcard/alertpanel/battery_frame.bin 电池模式最后显示的帧（PackBits，跨深睡 Fast 刷新）
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
