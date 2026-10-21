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
/sdcard/alertpanel/config.json      # 配置（schema_version=1；secrets 只有 *_ref）
/sdcard/alertpanel/alerts.db        # 告警库（JSONL，原子持久化）
/sdcard/alertpanel/cache/<src>.json # 各轮询源 2xx 原始响应缓存
/sdcard/fonts/...                   # 可选 UI 字体（.eefont/.bin，配置 admin.display.font）
```

SD 未挂载时：告警库降级为内存运行（重启丢失），状态页如实显示。

## 主机工具（firmware/tools/）

```sh
# 1) 静态自检：依赖方向/必需文件/禁止残留/许可头/partitions/sdkconfig/schema 一致性
python3 firmware/tools/validate_firmware.py            # 任一 FAIL 退出码 1

# 2) mock 告警源：8 场景（normal/batch/dup/clear/ttl/oversize/badlen/slow）
python3 firmware/tools/mock_alert_server.py --host 0.0.0.0 --port 8977
#   设备 sources[] 指向 http://<主机IP>:8977/<场景>；索引页 / 给出建议 SourceConfig

# 3) 真机串口 smoke（绝不刷写设备；需 pyserial）
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
- [ ] 连续运行（soak）：≥72h 无异常重启、内部 RAM 水位稳定（P3 项）

未真机验证项会在发布记录中如实标注（构建产物未刷入设备时，结论严格绑定
实体机镜像哈希，见仓库 `docs/VALIDATION.md`）。
