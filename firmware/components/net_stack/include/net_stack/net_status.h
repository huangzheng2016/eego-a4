// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// WiFi / 时间 / 外部告警库状态快照（设备状态页与 GET /api/v1/status 的数据源）。
// 所有字段由 net_task 单属主更新；读取方经 NetStack::queryStatus 拿快照。
// 数据源健康不在这里：dashboard 数据源（唯一取数模型）由 DashboardRuntime
// 的 data_pipeline 维护。

#include <cstddef>
#include <cstdint>
#include <string>

namespace eego::net {

struct WifiStatus {
  int state = 0;  // WifiState 枚举值（避免头文件依赖）
  std::string ssid;
  int rssi = 0;
  std::string ip;
  uint8_t last_reason = 0;
  uint32_t reconnect_backoff_ms = 0;  // 当前 WiFi 重连退避（指数退避档位）
};

// 统一时间源状态（net_task 用 time_fn/time_valid_fn 填充；SNTP 状态由
// TimeSync 提供）。valid=false 时 now 可能退化为 0 —— 展示层必须如实显示
// 「时间不可用」，不得把 0 时间伪装成有效告警时间。
struct TimeStatus {
  bool valid = false;        // 时间可信（组合根 time_valid_fn 判定）
  uint64_t now = 0;          // net_task 视角的统一 unix 秒（0 = 不可用）
  bool sntp_synced = false;  // 本次运行至少 SNTP 同步成功过一次
  uint64_t last_sync_at = 0; // 最近一次 SNTP 同步 unix 秒（0 = 从未）
};

// 外部告警库（POST /alerts 推送摄入的 alerts.db）持久化状态。
struct AlertDbStatus {
  bool attached = false;      // store 已接 alerts.db（SD 可用）
  size_t records = 0;         // 内存中的记录数
  size_t file_bytes = 0;      // alerts.db 当前大小（0 = 未知/不存在）
  uint32_t compactions = 0;   // 本次运行成功压缩次数
  uint64_t last_compact_at = 0;  // 最近一次成功压缩 unix 秒（0 = 从未）
  size_t io_failures = 0;     // 累计写/重命名失败次数
};

struct NetStatusSnapshot {
  WifiStatus wifi;
  uint32_t pool_sessions = 0;
  bool running = false;
  TimeStatus time;
  AlertDbStatus alert_db;
};

}  // namespace eego::net
