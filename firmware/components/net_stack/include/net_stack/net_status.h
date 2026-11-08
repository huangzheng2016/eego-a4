// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace eego::net {
struct WifiStatus {
  int state = 0;
  std::string ssid;
  int rssi = 0;
  std::string ip;
  uint8_t last_reason = 0;
  uint32_t reconnect_backoff_ms = 0;
  uint8_t last_disconnect_reason = 0;
  uint32_t reconnect_attempts = 0;
  uint32_t disconnects = 0;
  uint32_t driver_restarts = 0;
  uint32_t offline_ms = 0;
  uint32_t rssi_samples = 0;
  int rssi_min = 0;
  int rssi_avg = 0;
  uint32_t beacon_timeouts = 0;
  uint32_t roams = 0;
  int32_t last_dhcp_ms = -1;
  int tx_power_qdbm = 0;
};

struct TimeStatus {
  bool valid = false;
  uint64_t now = 0;
  bool sntp_synced = false;
  uint64_t last_sync_at = 0;
};

struct AlertDbStatus {
  bool attached = false;
  size_t records = 0;
  size_t file_bytes = 0;
  uint32_t compactions = 0;
  uint64_t last_compact_at = 0;
  size_t io_failures = 0;
};

struct NetStatusSnapshot {
  WifiStatus wifi;
  uint32_t pool_sessions = 0;
  bool running = false;
  TimeStatus time;
  AlertDbStatus alert_db;
};

}  // namespace eego::net
