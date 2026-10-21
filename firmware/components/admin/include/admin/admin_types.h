// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// admin 组件共享类型 —— 事件、回调签名、注入点（M17）。
//
// 并发模型（M12 定稿，本组件强制执行）：
//   * httpd handler（esp_http_server 任务上下文）只做解析/校验/限速/入队；
//   * AdminCommand → app_task 上下文执行（SD 写、NVS 清、重启、配置应用）；
//   * NetRequest / 告警摄入 → net_stack 命令队列 → net_task 上下文执行；
//   * 告警读 → SnapshotPublisher 无锁快照；告警写（ack/clear）→ AlertActionSink。
//
// 所有回调（事件/快照源/动作 sink）必须短平快，不得在调用方上下文做重活。

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "alert_core/snapshot.h"

namespace eego::admin {

// ---------------------------------------------------------------------------
// admin → app_task 事件（app_task 注册回调；典型用途：屏幕提示窗口开/关）
// ---------------------------------------------------------------------------
enum class AdminEventType : uint8_t {
  PortalOpened = 0,       // 配网门户窗口已开启（UI 应提示配网方法与剩余时间）
  PortalClosed,           // 门户窗口关闭（reason: expired/success/manual）
  ApFallbackOpened,       // 兜底 AP 已开启（管理 API 仍要认证）
  ApFallbackClosed,       // 兜底 AP 已关闭
  ProvisionSucceeded,     // 配网成功，NVS 已写入，设备即将重启
};

struct AdminEvent {
  AdminEventType type = AdminEventType::PortalOpened;
  const char* reason = "";        // PortalClosed 时: "expired"|"success"|"manual"
  uint32_t window_remaining_s = 0;  // 窗口剩余秒数（Opened 事件时有效）
};

using AdminEventCallback = void (*)(const AdminEvent& ev, void* ctx);

// ---------------------------------------------------------------------------
// 告警数据注入点（M18 组合根接线；M20 net_stack 提供实现）
//
// 与 net_stack/alert_actions.h 的 AlertActionSink 契约刻意保持同构
// （枚举顺序一致），M18 接线时可用薄适配层直接转发，无需本组件依赖
// 未合并的 M20 头文件。
// ---------------------------------------------------------------------------

// 快照源：返回当前不可变告警快照；未接线返回 nullptr（读端点回 503）。
using AlertSnapshotSource =
    std::function<std::shared_ptr<const alert_core::AlertSnapshot>()>;

enum class AlertActionKind : uint8_t {
  Ack = 0,    // 确认单条
  AckAll,     // 确认全部 Active
  Clear,      // 清除单条（state → cleared）
};

enum class AlertActionResultCode : uint8_t {
  Ok = 0,
  NotFound,      // 未知 id → 404
  InvalidState,  // 状态不允许（如对 cleared 记录 ack）→ 409
  InvalidAction,
  Unavailable,   // 队列满/超时 → 503
  NoStore,       // store 未附加 → 503
};

struct AlertActionResult {
  AlertActionResultCode code = AlertActionResultCode::Unavailable;
  size_t affected = 0;      // AckAll 时的确认计数
  std::string detail;       // 安全描述，可直接进错误信封
};

// 动作 sink：在 net_task 上下文执行 AlertStore 写；id 对 AckAll 忽略。
using AlertActionSink = std::function<AlertActionResult(
    AlertActionKind kind, const std::string& id, const std::string& ack_by)>;

// ---------------------------------------------------------------------------
// 统一错误码（M13 定稿错误信封 {"error":{"code","message","fields"?}}）
// ---------------------------------------------------------------------------
namespace errc {
inline constexpr const char* kInvalidRequest = "invalid_request";        // 400
inline constexpr const char* kValidationFailed = "validation_failed";    // 400
inline constexpr const char* kUnauthorized = "unauthorized";             // 401
inline constexpr const char* kForbidden = "forbidden";                   // 403
inline constexpr const char* kNotFound = "not_found";                    // 404
inline constexpr const char* kConflict = "conflict";                     // 409
inline constexpr const char* kPayloadTooLarge = "payload_too_large";     // 413
inline constexpr const char* kUnsupportedMediaType = "unsupported_media_type";  // 415
inline constexpr const char* kTooManyRequests = "too_many_requests";     // 429
inline constexpr const char* kInternal = "internal_error";               // 500
inline constexpr const char* kStoreFull = "store_full";                  // 503
inline constexpr const char* kUnavailable = "unavailable";               // 503
}  // namespace errc

}  // namespace eego::admin
