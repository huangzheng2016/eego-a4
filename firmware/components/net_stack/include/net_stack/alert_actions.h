// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// 告警动作契约（M11：POST /api/v1/alerts/{id}/ack、/alerts/ack-all、
// /alerts/{id}/clear；M12：AlertStore 只能由 net_task 写入）。
//
// httpd/UI 线程绝不直写 store：它们持 AlertActionSink 调用，动作经 NetStack
// 命令队列由 net_task 在 AlertStore 上下文执行，阻塞等明确结果（有超时）。
// admin（M17）用 NetStack::alertActionSink() / alertSnapshotSource() 拿到的
// 现成 functor 直接注入 setAlertActionSink(fn) / setAlertSnapshotSource(fn)；
// 未接线（net_task 未启动/未注入 publisher）时返回 Unavailable/nullptr，
// 可诊断、不崩溃（admin 端点映射 503）。

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "alert_core/snapshot.h"

namespace eego::net {

enum class AlertActionType : uint8_t {
  Ack = 0,  // 确认单条 Active → Acked（id 必填）
  AckAll,   // 确认全部 Active（id 忽略；0 条可确认也是成功，幂等）
  Clear,    // 清除单条 Active/Acked → Cleared（id 必填）
};

struct AlertAction {
  AlertActionType type = AlertActionType::Ack;
  std::string id;       // Ack/Clear 的目标记录 id；AckAll 忽略
  std::string ack_by;   // 操作者标识（"panel"、"api:<name>"…），Ack/AckAll 落库
                        // （store.clear 不记录操作者，Clear 时忽略本字段）
};

struct AlertActionResult {
  enum class Code {
    Ok = 0,         // 执行成功（Ack/Clear affected=1；AckAll affected=实际条数可为 0）
    NotFound,       // id 不存在
    InvalidState,   // 状态不允许：Ack 目标非 Active / Clear 目标已终态
    InvalidAction,  // 参数缺失：Ack/Clear 未带 id
    Unavailable,    // net_task 未运行/队列满/等待超时（admin 映射 503）
    NoStore,        // 未注入 AlertStore（防御性；正常 start 后不会出现）
  } code = Code::Unavailable;
  size_t affected = 0;  // AckAll: 实际确认条数；Ack/Clear: 1 或 0
  std::string detail;   // 人可读诊断（未接线/失败原因；日志与 503 body 用）
};

const char* toString(AlertActionResult::Code code);

// ---- admin 注入点（M17 setAlertSnapshotSource / setAlertActionSink 的 fn 形参）----
//
// AlertSnapshotSource：无锁读当前快照（SnapshotPublisher::acquire 的封装）。
//   返回 nullptr = 未接线或从未发布 —— admin 渲染空表/503，不得解引用。
// AlertActionSink：提交告警动作，net_task 执行并回明确结果。
//   net_task 未运行时 Code::Unavailable + detail（admin 映射 503）。
using AlertSnapshotSource = std::function<std::shared_ptr<const alert_core::AlertSnapshot>()>;
using AlertActionSink = std::function<AlertActionResult(const AlertAction&)>;

}  // namespace eego::net
