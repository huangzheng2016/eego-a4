// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

//
//

#include <cstdint>
#include <string>
#include <vector>

namespace panel {
struct RefreshPolicyConfig {
  uint32_t idle_after_min = 10;
  uint32_t idle_refresh_min = 5;
  bool critical_immediate = true;
  int timezone_offset_min = 0;
};

uint64_t nextRefreshBoundary(uint64_t unix_s, int timezone_offset_min, uint32_t interval_min);

uint64_t screenRotateEpoch(uint64_t unix_s, int64_t mono_ms, int timezone_offset_min,
                           uint32_t period_min);

bool screenRotateDue(uint64_t epoch, uint64_t last_epoch, int64_t mono_ms,
                     int64_t last_interaction_ms, uint32_t period_min, bool previous_displayed);

class RefreshPolicy {
 public:
  static constexpr uint64_t kMinValidUnix = 946684800ULL;  // 2000-01-01
  static constexpr uint64_t kBoundaryGraceS = 60;

  void configure(const RefreshPolicyConfig& config);
  const RefreshPolicyConfig& config() const { return config_; }

  void noteInteraction(int64_t mono_ms);
  void setHoldUntil(int64_t hold_until_ms) { hold_until_ms_ = hold_until_ms; }
  int64_t holdUntil() const { return hold_until_ms_; }
  bool idle(int64_t mono_ms) const;
  int64_t lastInteractionMs() const { return last_interaction_ms_; }

  bool allow(int64_t mono_ms, uint64_t unix_s, bool critical_arrived);

  void noteRendered(uint64_t unix_s, std::vector<std::string> displayed_critical);
  void noteRendered(uint64_t unix_s);

  bool criticalArrived(const std::vector<std::string>& active_critical) const;

  bool deferred() const { return due_mono_ms_ != 0; }
  uint64_t dueUnix() const { return due_unix_; }
  int64_t dueMonoMs() const { return due_mono_ms_; }

 private:
  RefreshPolicyConfig config_;
  int64_t last_interaction_ms_ = 0;
  int64_t hold_until_ms_ = 0;
  uint64_t due_unix_ = 0;
  int64_t due_mono_ms_ = 0;
  uint64_t last_render_unix_ = 0;
  std::vector<std::string> displayed_critical_;
};

}  // namespace panel
