// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// TimeSync —— WiFi 可用后的 SNTP 同步 + PCF8563 RTC 回写（M21）。
//
// 模型（M11「RTC+NTP（WiFi 上来后 sntp 同步）」+ M12 单属主）：
//   * 本对象由 net_task 独占驱动：onWifiConnected() 在 WiFi 刚连上的跃迁点
//     触发（重）同步；service() 在 net_task 主循环节拍里把同步结果回写 RTC；
//   * SNTP 同步通知回调跑在 lwip TCPIP 上下文，只做原子置位（短平快合同），
//     RTC I2C 写绝不发生在该上下文；
//   * 系统时间由 SNTP 协议栈同步成功时自动设置（settimeofday），本对象不重复
//     设置；RTC 回写让电池唤醒/重启后的离线时间保持可信（PCF8563 保持时间）。
//
// 线程安全：status() 任意上下文可读（原子字段）；其余接口只在 net_task 调用。

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace eego::net {

struct TimeSyncStatus {
  bool sntp_synced = false;   // 至少成功同步过一次（本次运行）
  uint64_t last_sync_at = 0;  // 最近一次 SNTP 同步 unix 秒（0 = 从未）
};

class TimeSync {
 public:
  // RTC 回写钩子：入参 unix 秒，返回是否写成功（组合根注入 PCF8563 写）。
  using RtcWriteFn = std::function<bool(uint64_t unix_now)>;

  // servers 为空用默认（cn.pool.ntp.org / pool.ntp.org，按
  // CONFIG_LWIP_SNTP_MAX_SERVERS 截断——本工程为 1，实配首个）。rtc_write
  // 可空（无 RTC 时纯 SNTP；同步状态仍可查询）。幂等可重配。
  void configure(std::vector<std::string> servers, RtcWriteFn rtc_write);

  // WiFi 刚连上（net_task 上下文）：首次初始化 esp_sntp，此后 stop+init 强制
  // 立即重新同步（SNTP_OPMODE_POLL 每小时自同步，重连不等下一拍）。
  void onWifiConnected();

  // net_task 主循环节拍：挂起的 RTC 回写在这里落地（net_task 上下文做 I2C）。
  void service();

  TimeSyncStatus status() const {
    TimeSyncStatus s;
    s.sntp_synced = synced_.load(std::memory_order_relaxed);
    s.last_sync_at = last_sync_at_.load(std::memory_order_relaxed);
    return s;
  }

  // SNTP 同步通知落地（仅本组件 C 回调调用；原子写，任意上下文安全）。
  void noteSync(uint64_t unix_sec);

 private:
  std::vector<std::string> servers_;
  RtcWriteFn rtc_write_;
  bool sntp_initialized_ = false;

  std::atomic<bool> synced_{false};
  std::atomic<bool> rtc_write_pending_{false};
  std::atomic<uint64_t> last_sync_at_{0};
};

}  // namespace eego::net
