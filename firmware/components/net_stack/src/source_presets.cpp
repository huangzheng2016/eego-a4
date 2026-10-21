// SPDX-License-Identifier: GPL-3.0-or-later
// 来源预设/映射模板实现（M16 任务 4）。
//
// 模板即 SourceConfig 起点（toJson 序列化），无厂商私有解析代码；
// 需要自定义脚本的来源在 notes 写明脚本输出契约（规范事件数组，M11 §4）。
#include "net_stack/source_presets.h"

namespace eego::net {

using alert_core::JsonValue;

// 恒等映射模板（自定义脚本场景兜底）：脚本直接产规范事件数组。
static SourceConfig makeIdentityTemplate(const std::string& id, const std::string& url) {
  SourceConfig c;
  c.id = id;
  c.kind = "http_poll";
  c.url = url;
  c.array_path = "";  // 根数组/单对象
  c.field_map = {};   // 恒等映射
  c.severity_map = {};
  c.clear_after_misses = kSourceClearMissesDefault;
  return c;
}

static std::vector<SourcePreset> buildPresets() {
  std::vector<SourcePreset> v;

  // ---- Home Assistant：推荐 http_push（HA 无通用告警查询 API，不伪装支持）----
  {
    SourcePreset p;
    p.id = "home_assistant";
    p.label_zh = "Home Assistant";
    p.label_en = "Home Assistant";
    p.kind_hint = "http_push";
    p.builtin_poll = false;
    p.notes =
        "HA 没有通用的“告警列表”查询 API（/api/states 是全量状态，语义因户而异），"
        "因此不伪装支持轮询。标准做法：在 HA 自动化 action 里用 REST Command / "
        "webhook 把告警 POST 到面板 http_push（POST /api/v1/alerts，头 X-Ingest-Key，"
        "体为规范事件 JSON：type/title/message/fields，severity 三档）。"
        "示例：alias: 面板告警 → action: rest_command.eego_alert，"
        "data: {type: \"home.water_leak\", severity: \"critical\", title: \"...\", fields: {...}}。"
        "面板侧无需 sources[] 配置，ingest key 由 admin 组件管理。";
    p.notes_en =
        "HA has no generic \"alert list\" query API (/api/states is full state with "
        "per-household semantics), so polling is not faked. Standard approach: in an "
        "HA automation action, use a REST Command / webhook to POST alerts to the "
        "panel http_push endpoint (POST /api/v1/alerts, header X-Ingest-Key, body = "
        "canonical event JSON: type/title/message/fields, three severity levels). "
        "Example: alias: panel alert -> action: rest_command.eego_alert, "
        "data: {type: \"home.water_leak\", severity: \"critical\", title: \"...\", "
        "fields: {...}}. No sources[] config is needed on the panel; the ingest key "
        "is managed by the admin component.";
    v.push_back(std::move(p));
  }

  // ---- Grafana Alerting：推荐 http_push（告警联系点 webhook）----
  {
    SourcePreset p;
    p.id = "grafana";
    p.label_zh = "Grafana Alerting";
    p.label_en = "Grafana Alerting";
    p.kind_hint = "http_push";
    p.builtin_poll = false;
    p.notes =
        "Grafana Alerting 的标准集成是 Contact point → webhook 指向面板 http_push"
        "（POST /api/v1/alerts，头 X-Ingest-Key）。Grafana 默认 POST 体不是规范事件，"
        "在 Grafana 通知模板（message/template）里把 alerts[] 渲染成规范事件 JSON："
        "type←labels.alertname（或固定 svc.http_down 等注册表类型）、"
        "severity←labels.severity、title←annotations.summary、message←annotations.description、"
        "fields←labels。面板侧无需 sources[] 配置。";
    p.notes_en =
        "The standard Grafana Alerting integration is a Contact point webhook "
        "pointing at the panel http_push endpoint (POST /api/v1/alerts, header "
        "X-Ingest-Key). Grafana's default POST body is not a canonical event; "
        "render alerts[] into canonical event JSON in the Grafana notification "
        "template: type<-labels.alertname (or a fixed registry type such as "
        "svc.http_down), severity<-labels.severity, title<-annotations.summary, "
        "message<-annotations.description, fields<-labels. No sources[] config "
        "is needed on the panel.";
    v.push_back(std::move(p));
  }

  // ---- Prometheus：/api/v1/alerts 是稳定 JSON API，开箱可解析 ----
  {
    SourcePreset p;
    p.id = "prometheus";
    p.label_zh = "Prometheus 告警";
    p.label_en = "Prometheus alerts";
    p.kind_hint = "http_poll";
    p.builtin_poll = true;
    SourceConfig c;
    c.id = "prometheus";
    c.url = "http://prometheus.local:9090/api/v1/alerts";
    c.interval_ms = 60000;
    c.array_path = "data.alerts";
    c.field_map = {{"type", "labels.alertname"},
                   {"severity", "labels.severity"},
                   {"title", "annotations.summary"},
                   {"message", "annotations.description"},
                   {"fields", "labels"},
                   {"state", "state"}};
    c.severity_map = {{"critical", "critical"},
                      {"warning", "warning"},
                      {"info", "info"},
                      {"none", "info"}};
    c.clear_states = {"inactive"};  // Prometheus 的清除信号是 state=inactive
    c.dedup_key = {"alertname"};
    c.clear_after_misses = 3;
    p.config_template = c.toJson();
    p.notes =
        "Prometheus HTTP API /api/v1/alerts 是稳定 JSON 接口，模板开箱可用。"
        "labels 整体进 fields（≤16 键预算，超限键被跳过）；severity 经 severity_map 映射；"
        "state=inactive 经 clear_states 映射为清除（firing/pending 保持活动）。"
        "开启认证时把 token 存 NVS（admin 写 sec 命名空间）并填 auth_token_ref。";
    p.notes_en =
        "The Prometheus HTTP API /api/v1/alerts is a stable JSON interface; the "
        "template works out of the box. labels go wholesale into fields (16-key "
        "budget, excess keys skipped); severity is mapped via severity_map; "
        "state=inactive maps to clear via clear_states (firing/pending stay "
        "active). When auth is enabled, store the token in NVS (admin writes "
        "the sec namespace) and fill auth_token_ref.";
    v.push_back(std::move(p));
  }

  // ---- NAS/主机：厂商各异，自定义脚本 + 恒等映射模板（不伪装支持）----
  {
    SourcePreset p;
    p.id = "nas_generic";
    p.label_zh = "NAS / 服务器（自定义脚本）";
    p.label_en = "NAS / server (custom script)";
    p.kind_hint = "http_poll";
    p.builtin_poll = false;
    p.config_template = makeIdentityTemplate("nas-poll", "http://nas.local:8888/alerts.json")
                            .toJson();
    p.notes =
        "Synology/QNAP/TrueNAS/Windows 的告警 API 各不相同，多数没有稳定的 JSON 告警端点，"
        "因此不提供厂商绑定模板、不伪装支持。推荐：NAS 上跑自定义脚本（计划任务），"
        "把告警转成规范事件数组（M11 §4 schema：[{type,severity,title,message,fields,...}]），"
        "用本模板轮询（恒等映射）；也可以直接 http_push 免轮询。"
        "脚本输出契约：HTTP 200 + JSON 数组，type 必填，severity 三档，title≤120，"
        "message≤2000，fields≤16 键。";
    p.notes_en =
        "Synology/QNAP/TrueNAS/Windows alert APIs all differ, and most lack a "
        "stable JSON alert endpoint, so no vendor-bound template is offered and "
        "support is not faked. Recommended: run a custom script on the NAS "
        "(scheduled task) that converts alerts into the canonical event array "
        "(M11 §4 schema: [{type,severity,title,message,fields,...}]) and poll "
        "it with this template (identity mapping); direct http_push also works. "
        "Script output contract: HTTP 200 + JSON array, type required, three "
        "severity tiers, title<=120, message<=2000, fields<=16 keys.";
    v.push_back(std::move(p));
  }

  // ---- OpenWrt/路由器：ubus 无通用告警接口，自定义脚本模板 ----
  {
    SourcePreset p;
    p.id = "openwrt";
    p.label_zh = "OpenWrt / 路由器（自定义脚本）";
    p.label_en = "OpenWrt / router (custom script)";
    p.kind_hint = "http_poll";
    p.builtin_poll = false;
    p.config_template = makeIdentityTemplate("router-poll", "http://192.168.1.1:8888/alerts.json")
                            .toJson();
    p.notes =
        "OpenWrt 没有通用告警 API（ubus 需 root + Lua 且事件源格式随软件包变化），"
        "不伪装支持。推荐：路由器上跑自定义脚本（netifd/系统日志事件 → 规范事件数组），"
        "或路由器上 SSH 到 LAN 内某主机输出。脚本契约同 nas_generic。"
        "常见类型映射参考：掉线→net.device_offline，拨号失败→net.ping_loss，"
        "CPU/内存高→host.cpu_high/host.mem_high。";
    p.notes_en =
        "OpenWrt has no generic alert API (ubus requires root + Lua and event "
        "formats vary by package), so support is not faked. Recommended: run a "
        "custom script on the router (netifd/system log events -> canonical "
        "event array), or have the router SSH into a LAN host that emits them. "
        "Script contract same as nas_generic. Common type mapping: link down -> "
        "net.device_offline, dial failure -> net.ping_loss, high CPU/memory -> "
        "host.cpu_high/host.mem_high.";
    v.push_back(std::move(p));
  }

  // ---- UPS/NUT：文本协议，不伪装直连，自定义脚本模板 ----
  {
    SourcePreset p;
    p.id = "ups_nut";
    p.label_zh = "UPS / NUT（自定义脚本）";
    p.label_en = "UPS / NUT (custom script)";
    p.kind_hint = "http_poll";
    p.builtin_poll = false;
    p.config_template = makeIdentityTemplate("ups-poll", "http://ups-host.local:8888/alerts.json")
                            .toJson();
    p.notes =
        "NUT（upsc/upscmd）是文本协议，没有 HTTP JSON 接口，不伪装支持直连。"
        "推荐：UPS 服务器上跑脚本（upsc <ups> 轮询 → 阈值判断 → 输出规范事件数组）："
        "ups.on_battery（市电中断，critical）、ups.battery_low（电池低，warning/critical）、"
        "ups.overload（过载，critical）；恢复时由 clear_after_misses 自动清除，"
        "或脚本显式发 state=cleared 事件。脚本契约同 nas_generic。";
    p.notes_en =
        "NUT (upsc/upscmd) is a text protocol with no HTTP JSON interface; direct "
        "connection is not faked. Recommended: run a script on the UPS server "
        "(poll upsc <ups> -> threshold check -> emit canonical event array): "
        "ups.on_battery (mains loss, critical), ups.battery_low (low battery, "
        "warning/critical), ups.overload (overload, critical); recovery "
        "auto-clears via clear_after_misses, or the script explicitly sends a "
        "state=cleared event. Script contract same as nas_generic.";
    v.push_back(std::move(p));
  }

  // ---- 自定义 JSON：恒等映射兜底，所有"脚本直接产规范事件"场景 ----
  {
    SourcePreset p;
    p.id = "custom_json";
    p.label_zh = "自定义 JSON 端点";
    p.label_en = "Custom JSON endpoint";
    p.kind_hint = "http_poll";
    p.builtin_poll = true;
    p.config_template =
        makeIdentityTemplate("custom-poll", "http://example.local/alerts.json").toJson();
    p.notes =
        "通用兜底模板：端点直接返回规范事件数组（M11 §4 schema），恒等映射，"
        "无需 field_map/severity_map。需要字段改名/严重度映射时在管理 UI 加 field_map "
        "（canonical←源路径，'.' 分层）与 severity_map。认证用 auth_token_ref "
        "（token 存 NVS sec 命名空间）；自签 TLS 显式 tls_verify=false。";
    p.notes_en =
        "Generic fallback template: the endpoint returns the canonical event "
        "array (M11 §4 schema) directly, identity mapping, no field_map/"
        "severity_map needed. Add field_map (canonical <- source path, '.' "
        "layered) and severity_map in the admin UI when renaming fields or "
        "mapping severities. Auth via auth_token_ref (token stored in the NVS "
        "sec namespace); self-signed TLS requires explicit tls_verify=false.";
    v.push_back(std::move(p));
  }

  return v;
}

const std::vector<SourcePreset>& sourcePresets() {
  static const std::vector<SourcePreset> presets = buildPresets();
  return presets;
}

const SourcePreset* findSourcePreset(const std::string& id) {
  for (const auto& p : sourcePresets()) {
    if (p.id == id) return &p;
  }
  return nullptr;
}

}  // namespace eego::net
