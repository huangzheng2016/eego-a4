// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// 源健康统计与 WiFi 状态快照（状态页 S3 / GET /api/v1/status 数据源，M11 §1）。
// 所有字段由 net_task 单属主更新；读取方经 NetStack::queryStatus 拿快照。

#include <cstdint>
#include <string>
#include <vector>

namespace eego::net {

struct SourceHealth {
  std::string id;
  std::string kind;
  bool enabled = true;
  uint32_t interval_ms = 60000;

  // 轮询节律（unix 秒，0 = 从未）
  uint64_t last_attempt_at = 0;
  uint64_t last_success_at = 0;
  uint64_t next_due_at = 0;

  // 失败退避：连续失败次数与当前退避档（源级指数退避 = interval << failures，封顶）
  uint32_t consecutive_failures = 0;
  uint32_t backoff_level = 0;

  int last_http_status = 0;
  std::string last_error;  // 不含秘密/凭据

  uint32_t events_ingested = 0;
  uint32_t events_cleared = 0;
  uint32_t events_skipped = 0;  // 预算超限/映射缺字段被丢弃的事件数

  // 离线缓存（最后一次成功响应）
  uint64_t cache_fetched_at = 0;
  uint32_t cache_bytes = 0;
};

struct WifiStatus {
  int state = 0;  // WifiState 枚举值（避免头文件依赖）
  std::string ssid;
  int rssi = 0;
  std::string ip;
  uint8_t last_reason = 0;
  uint32_t reconnect_backoff_ms = 0;  // 当前 WiFi 重连退避（指数退避档位）
};

// 统一时间源状态（M21：net_task 用 time_fn/time_valid_fn 填充；SNTP 状态由
// TimeSync 提供）。valid=false 时 now 可能退化为 0 —— 展示层必须如实显示
// 「时间不可用」，不得把 0 时间伪装成有效告警时间。
struct TimeStatus {
  bool valid = false;        // 时间可信（组合根 time_valid_fn 判定）
  uint64_t now = 0;          // net_task 视角的统一 unix 秒（0 = 不可用）
  bool sntp_synced = false;  // 本次运行至少 SNTP 同步成功过一次
  uint64_t last_sync_at = 0; // 最近一次 SNTP 同步 unix 秒（0 = 从未）
};

struct NetStatusSnapshot {
  WifiStatus wifi;
  std::vector<SourceHealth> sources;
  uint32_t pool_sessions = 0;
  bool running = false;
  TimeStatus time;
};

}  // namespace eego::net
