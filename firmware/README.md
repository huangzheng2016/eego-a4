# EEGO A4 固件

当前主产品是基于 ESP-IDF 的可配置告警/数据面板固件，无 Arduino 依赖。
HTTP 数据源经声明式解析后驱动 card、chart、table、text、image、button、alert
七类 panel；复杂转换放在用户服务器完成，设备不执行任意脚本。

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
3. 通过串口 `net` 或路由器 DHCP 列表找到 IP，打开 `http://<device-ip>/` 登录。
4. 在「数据面板」导入 JSON，先校验再应用；导入只改草稿，应用后才生效。

`http://<device-ip>/` 是一个加载器：从 GitHub Pages 拉取管理 Web 应用
（`EEGO_WEB_APP_BASE`，可在编译时覆盖），应用在设备同源下调用 `/api/v1`。
无法访问互联网时打开 `http://<device-ip>/legacy` 使用内置管理页；配网门户始终内置。

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

## 数据面板

- 数据源支持标准 JSON、声明式 JSONPath、Prometheus、文本和 PNG/JPEG。
- metric 使用 `metric/value/unit/timestamp_ms/labels`；事件使用
  `id/rule_id/timestamp_ms/severity/state/title/message/linked_panel_id`。
- 页面使用 grid 布局；可配置变量、时间窗口、阈值、告警规则、linked panel 和
  source/panel 独立调度。表达式和按钮 action 都是白名单，不是脚本。
- 布局规则（设备、固件校验、主机校验共用）：`grid_columns` 按比例映射到屏幕
  宽度（任意列数和边界都可）；每一屏固定 8 个 grid 行，按比例映射到屏幕高度；
  可见 panel 不得重叠，且必须完整落在一屏内（`h ≤ 8` 且 `y % 8 + h ≤ 8`），
  第 n 屏对应 `y` 在 `[8n, 8n+8)`。`row_height` 仅为兼容字段，不影响设备几何。
  旧版 SD 配置中跨屏/重叠的 panel 会在加载时自动挪到下一个空位。
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
| `GET/PUT /api/v1/config` | 非秘密管理配置 |
| `GET/PUT/DELETE /api/v1/dashboard` | 读取、校验后替换或删除 bundle |
| `POST /api/v1/dashboard/validate` | 只校验候选 bundle，不应用 |
| `POST /api/v1/dashboard/sources/test` | 测试草稿数据源，返回归一化预览 |
| `POST /api/v1/dashboard/sources/probe` | 抓取一次原始响应（敏感键值打码、≤16 KiB），用于挑选 JSONPath |
| `GET /api/v1/dashboard/templates` | 内置预设库与完整示例 |
| `GET /api/v1/display/snapshot.bmp` | 当前屏幕帧（552×768 1-bpp BMP）；未接线时 503 |
| `GET/PUT /api/v1/settings` | 本机设置；schema 在同路径加 `/schema` |
| `GET/PUT/DELETE /api/v1/sources/credentials` | 管理数据源凭据 |
| `GET/PUT /api/v1/input-bindings` | 自定义输入；另有 `/schema` 和 `/reset` |
| `POST /api/v1/system/reboot` | 重启，需 `confirm:"REBOOT"` |

`POST /api/v1/alerts` 使用独立的 `X-Ingest-Key` 提交规范事件，支持单条和批量；
ingest key 不能读取管理资源。具体字段以设备 schema 和校验错误为准。

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

同 key PUT 即轮换；仍被告警 sources、dashboard sources 或 plans 引用时不能删除。
缺失或非法引用会使请求失败，不会降级成无认证请求。原始 2xx 响应会按设计缓存，
服务器只应返回面板需要的最小数据，配置存引用不等于缓存中没有秘密。

## 输入、BLE 与电源

- Up/Down 短按移动焦点、长按翻页；Power 短按激活、长按返回；Front 短按 Home、
  长按 Help、双击 Status。触控支持 tap 和水平 swipe。
- 自定义绑定最多 24 条，只能引用白名单 builtin 或已注册 panel action。
  Power、Front Home、确认和恢复流程不可重映射。
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
/sdcard/alertpanel/alerts.db         告警记录
/sdcard/alertpanel/cache/            告警源响应缓存
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
python3 firmware/tools/mock_alert_server.py --host 0.0.0.0 --port 8977
python3 firmware/tools/smoke_serial_test.py --port <PORT> \
  --admin-password '<管理密码>' --mock-url http://<主机IP>:8977/normal
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
