// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// 统一时间源（M21）—— 面板/net_task/告警库共用的唯一墙钟。
//
// 优先级（M11「RTC+NTP（WiFi 上来后 sntp 同步）」）：
//   1. 系统时间可信（≥ eego::net::kPlausibleUnixFloor，SNTP 已同步或启动时
//      已从 RTC 设置）→ time(nullptr)：SNTP 每小时自同步保持准确，TLS 证书
//      校验同源；
//   2. RTC（PCF8563）读数有效（VL 未置位且字段范围合法）→ RTC epoch：电池唤醒/离线兜底；
//   3. 都不可用 → now() 返回退化值（0 附近）且 valid()=false。
//
// 诚实性合同：valid()=false 时状态页/状态栏如实显示「时间不可用」，
// 绝不把 0 时间伪装成有效告警时间（告警时间戳字段显示 "--"、年龄显示"从未"）。
//
// 调用上下文：app_task（面板状态栏/年龄）与 net_task（time_fn，ingest/expire/
// 快照 generated_at）都会调用；legacy I2C 驱动端口级锁保证 RTC 共享安全。
// RTC 实例为函数内静态（无状态驱动句柄），与 app_main/panel 内的实例等同。

#include <cstdint>

namespace panel {

class TimeSource {
 public:
  // 当前 unix 秒（可信系统时间 > RTC > 退化值）。net_task time_fn 注入点。
  static uint64_t now();

  // 时间是否可信（系统时间可信或 RTC 有效）。net_task time_valid_fn 注入点。
  static bool valid();

  // 启动时调用一次：RTC 有效而系统时间不可信 → settimeofday 把系统时间设为
  // RTC（TLS 证书校验/日志/SNTP 前的摄入都依赖系统时间）。返回是否设置了。
  static bool syncSystemFromRtc();

  // RTC 回写（M21：SNTP 同步成功后由 net_task 经 NetStackConfig.rtc_write_fn
  // 调用；unix → PCF8563）。无 RTC/写失败返回 false（如实，不重试风暴）。
  static bool writeRtc(uint64_t unix_now);
};

}  // namespace panel
