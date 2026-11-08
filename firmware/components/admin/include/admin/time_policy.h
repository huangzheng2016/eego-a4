// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// 浏览器校时策略（纯函数，可主机单测）：SNTP 始终优先。
//   * 设备时间无效 → 采用；
//   * 未经 SNTP 同步且偏差 > 2 s → 采用；
//   * 24 小时内 SNTP 同步过 → 不覆盖。

#include <cstdint>

namespace eego::admin {

constexpr int64_t kBrowserTimeMinDriftS = 2;
constexpr int64_t kSntpTrustS = 86400;

inline bool shouldApplyBrowserTime(bool time_valid, bool sntp_synced, int64_t last_sync_age_s,
                                   int64_t drift_s) {
  if (!time_valid) return true;
  if (sntp_synced && last_sync_age_s >= 0 && last_sync_age_s < kSntpTrustS) return false;
  const int64_t abs_drift = drift_s < 0 ? -drift_s : drift_s;
  return abs_drift > kBrowserTimeMinDriftS;
}

}  // namespace eego::admin
