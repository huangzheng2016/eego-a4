// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// http_push 摄入桥接（M16 任务 6）—— 供 admin 组件 httpd 上下文调用。
//
// 边界（M13 定稿）：
//   * 本函数在 httpd 任务上下文执行，只做 解析/校验/入队/等待，
//     绝不直接触碰 AlertStore（单属主：store 写入只发生在 net_task）；
//   * X-Ingest-Key 认证、限流、错误信封由 admin 层负责；
//   * 返回码即 HTTP 语义映射输入：Ok→201/202（按 created/updated）、
//     BadRequest→400（error 为字段级）、Oversize→413、Unavailable/StoreFull→503。
//
// 体格式：单条事件对象或 {"alerts": [...]}（≤32 条），M11 §4 schema；
// http_push 记录永不自动清除，只等显式 clear 或 TTL（M11 §2）。
//
// 调用示例（admin httpd handler 内）：
//   auto r = eego::net::httpPushIngest(body, client_ip);
//   if (r.code == PushResult::Code::Ok) return httpd_resp_send_201_or_202(...);

#include <string>

#include "net_stack/net_task.h"

namespace eego::net {

// sender 为来源标识（如客户端 IP 或 key 名），存入 record.source.ref；
// 永不写入日志的秘密（X-Ingest-Key 本身不落此处）。
inline NetStack::PushResult httpPushIngest(const std::string& body,
                                           const std::string& sender,
                                           uint32_t wait_ms = 5000) {
  return NetStack::instance().pushIngest(body, sender, wait_ms);
}

}  // namespace eego::net
