// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

//
//

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace eego::net {
constexpr uint64_t kPlausibleUnixFloor = 1704067200ULL;  // 2024-01-01T00:00:00Z

struct TimeSyncStatus {
  bool sntp_synced = false;
  uint64_t last_sync_at = 0;
};

class TimeSync {
 public:
  using RtcWriteFn = std::function<bool(uint64_t unix_now)>;

  void configure(std::vector<std::string> servers, RtcWriteFn rtc_write);

  void onWifiConnected();

  void service();

  TimeSyncStatus status() const {
    TimeSyncStatus s;
    s.sntp_synced = synced_.load(std::memory_order_relaxed);
    s.last_sync_at = last_sync_at_.load(std::memory_order_relaxed);
    return s;
  }

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
