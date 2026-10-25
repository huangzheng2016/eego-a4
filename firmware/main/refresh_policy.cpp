// SPDX-License-Identifier: GPL-3.0-or-later
// 空闲感知刷新节奏（契约见 refresh_policy.h）。
#include "refresh_policy.h"

#include <algorithm>
#include <utility>

namespace panel {

uint64_t nextRefreshBoundary(uint64_t unix_s, int timezone_offset_min, uint32_t interval_min) {
  const int64_t period = static_cast<int64_t>(interval_min == 0 ? 1 : interval_min) * 60;
  const int64_t offset = static_cast<int64_t>(timezone_offset_min) * 60;
  const int64_t local = static_cast<int64_t>(unix_s) + offset;
  int64_t slot = local / period;
  if (local % period < 0) --slot;  // floor（负本地时间理论上不出现，保持正确）
  const int64_t next_local = (slot + 1) * period;
  return static_cast<uint64_t>(next_local - offset);
}

void RefreshPolicy::configure(const RefreshPolicyConfig& config) {
  const bool boundary_changed = config.idle_refresh_min != config_.idle_refresh_min ||
                                config.timezone_offset_min != config_.timezone_offset_min;
  config_ = config;
  // 间隔/时区变化 → 重新登记边界（下次 allow 时按新配置计算）。
  if (boundary_changed) {
    due_unix_ = 0;
    due_mono_ms_ = 0;
  }
}

void RefreshPolicy::noteInteraction(int64_t mono_ms) {
  last_interaction_ms_ = mono_ms;
  due_unix_ = 0;
  due_mono_ms_ = 0;
}

bool RefreshPolicy::idle(int64_t mono_ms) const {
  if (config_.idle_after_min == 0) return false;
  return mono_ms - last_interaction_ms_ >= static_cast<int64_t>(config_.idle_after_min) * 60000;
}

bool RefreshPolicy::allow(int64_t mono_ms, uint64_t unix_s, bool critical_arrived) {
  if (!idle(mono_ms)) {
    due_unix_ = 0;
    due_mono_ms_ = 0;
    return true;
  }
  if (critical_arrived && config_.critical_immediate) return true;
  const uint32_t interval_min = config_.idle_refresh_min == 0 ? 1 : config_.idle_refresh_min;
  const int64_t interval_ms = static_cast<int64_t>(interval_min) * 60000;
  const bool time_valid = unix_s >= kMinValidUnix;
  if (due_mono_ms_ == 0) {
    if (time_valid) {
      due_unix_ = nextRefreshBoundary(unix_s, config_.timezone_offset_min, interval_min);
      const uint64_t latest = due_unix_ - static_cast<uint64_t>(interval_min) * 60;
      if (unix_s - latest < kBoundaryGraceS && last_render_unix_ < latest) due_unix_ = latest;
      // 单调上限：边界距离 + 1 个间隔余量（墙钟回拨/跳变时仍会刷新）。
      due_mono_ms_ = mono_ms + static_cast<int64_t>(due_unix_ - unix_s) * 1000 + interval_ms;
    } else {
      due_unix_ = 0;
      due_mono_ms_ = mono_ms + interval_ms;
    }
    if (due_mono_ms_ == 0) due_mono_ms_ = 1;
  }
  if (due_unix_ != 0 && time_valid && unix_s >= due_unix_) return true;
  return mono_ms >= due_mono_ms_;
}

void RefreshPolicy::noteRendered(uint64_t unix_s, std::vector<std::string> displayed_critical) {
  displayed_critical_ = std::move(displayed_critical);
  std::sort(displayed_critical_.begin(), displayed_critical_.end());
  noteRendered(unix_s);
}

void RefreshPolicy::noteRendered(uint64_t unix_s) {
  last_render_unix_ = unix_s >= kMinValidUnix ? unix_s : 0;
  due_unix_ = 0;
  due_mono_ms_ = 0;
}

bool RefreshPolicy::criticalArrived(const std::vector<std::string>& active_critical) const {
  if (!config_.critical_immediate) return false;
  for (const std::string& id : active_critical) {
    if (!std::binary_search(displayed_critical_.begin(), displayed_critical_.end(), id)) {
      return true;
    }
  }
  return false;
}

}  // namespace panel
