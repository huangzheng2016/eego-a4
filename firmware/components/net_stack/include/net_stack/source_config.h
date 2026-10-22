// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// 告警源配置（config.json "sources" 数组元素，M11 §5 骨架 + M16 通用映射扩展）。
//
// 通用 http_poll 源形状：
//   { "id": "nas-poll", "kind": "http_poll", "enabled": true,
//     "url": "http://nas.local/api/alerts", "interval_ms": 60000,
//     "auth_token_ref": "nvs:source/nas_token", "tls_verify": true,
//     "clear_after_misses": 3,
//     "array_path": "data.alerts",
//     "field_map":   { "severity": "labels.severity", "title": "annotations.summary" },
//     "severity_map":{ "error": "critical", "warn": "warning" },
//     "clear_states": ["inactive"],
//     "dedup_key": ["host"], "default_type": "generic.any",
//     "headers": { "X-Extra": "v" }, "timeout_ms": 20000 }
//
// field_map 把源 JSON 路径映射到规范事件键：type/severity/title/message/
// fields/state/ttl_s/fingerprint/ts；未列出的键用恒等映射（"title"←"title"）。
// 路径用 '.' 分层（"labels.severity"），支持数组下标（"data.alerts.0"）。
// severity_map 把源严重度文本映射到 canonical（critical|warning|info）；
// 未命中且原文非法时缺省交给类型注册表（has_severity=false）。
//
// 校验：fromJson 做字段级强类型/范围校验（错误经 ValidationError 上报，
// admin 层映射 400）；kind 由适配器注册表裁决，未知 kind 在这里不报错
// （注册点可后装），addSource 时报 "unknown kind"。

#include <map>
#include <string>
#include <vector>

#include "alert_core/types.h"
#include "net_stack/http_pool.h"

namespace eego::net {

constexpr uint32_t kSourceIntervalMinMs = 10000;    // M11：最小 10s
constexpr uint32_t kSourceIntervalDefaultMs = 60000;  // M11：默认 60s
constexpr uint32_t kSourceIntervalMaxMs = 3600000;
constexpr uint32_t kSourceTimeoutDefaultMs = 20000;
constexpr uint32_t kSourceClearMissesDefault = 3;  // M11：默认 3，0 = 从不自动清除

struct SourceConfig {
  std::string id;                  // [a-z0-9_-]{1,64}，兼作 cache 文件名（防路径穿越）
  std::string kind = "http_poll";  // http_poll | 后装适配器 kind
  bool enabled = true;
  std::string url;                 // http_poll：完整 URL
  uint32_t interval_ms = kSourceIntervalDefaultMs;
  std::string auth_token_ref;      // "nvs:source/<key>"；空 = 无认证
  bool tls_verify = true;          // false = 内网自签（显式关闭才生效）
  uint32_t clear_after_misses = kSourceClearMissesDefault;
  std::string array_path;          // 事件数组路径；空 = 根（根数组或单对象）
  std::map<std::string, std::string> field_map;    // canonical key -> 源 JSON 路径
  std::map<std::string, std::string> severity_map;  // 源严重度 -> canonical
  // 视为"已清除"的 state 值列表（如 Prometheus "inactive"、脚本 "resolved"）；
  // 空 = 默认 {"cleared"}。清除语义其余部分由 clear_after_misses 承担。
  std::vector<std::string> clear_states;
  std::vector<std::string> dedup_key;  // 空 = 类型注册表默认 → 内置默认
  std::string default_type;        // 映射缺 type 时的兜底；空 = generic.any
  std::vector<NetHeader> headers;  // 非敏感静态头；秘密字面值一律拒绝
  std::vector<NetHeader> header_secret_refs;  // 头名 -> nvs:source/<key>
  uint32_t timeout_ms = kSourceTimeoutDefaultMs;
  // 响应体解析上限（JSON DOM 在内部 RAM，给池的 512KB 传输上限之外再设一道
  // 解析预算；超限记源错误，绝不硬吃）。默认 64KB，可调至 512KB。
  uint32_t max_body_bytes = 64 * 1024;

  // 字段级解析校验（alert_core::ValidationError 语义：field 相对 sources[i] 元素）。
  static bool fromJson(const alert_core::JsonValue& obj, SourceConfig& out,
                       alert_core::ValidationError& err);
  alert_core::JsonValue toJson() const;
  // 语义校验（id 字符集/URL scheme/interval 范围）；返回空串 = 通过。
  std::string validate() const;
};

// 在实际请求时解析全部 source 凭据；失败时 out 不可用于传输。
bool buildSourceRequest(const SourceConfig& cfg, NetRequest& out, std::string& error);

}  // namespace eego::net
