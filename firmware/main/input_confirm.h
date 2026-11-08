// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

#include "input_events.h"
#include "alert_core/snapshot.h"

namespace panel {
inline constexpr int64_t kConfirmWindowUs = 5000000;

enum class ConfirmSource : uint8_t { None, Up, Down, Power, Touch, Front };

struct ConfirmTarget {
  uint64_t page_epoch = 0;
  uint64_t snapshot_epoch = 0;
  std::string control;
  std::string record_id;
  bool physical_power_only = false;
};

enum class ConfirmRecordRule : uint8_t { Mute, Clear };

bool confirmRecordTargetCurrent(const ConfirmTarget& target, ConfirmRecordRule rule,
                                const alert_core::AlertSnapshot* snapshot);

class FreshPressConfirm {
 public:
  void arm(const ConfirmTarget& target, int64_t now_us);
  void cancel();

  bool beginPress(ConfirmSource source, const std::string& control, int64_t press_us,
                  int64_t now_us);
  void noteRelease(ConfirmSource source, int64_t release_us);
  bool completePress(ConfirmSource source, int64_t now_us) const;
  bool expired(int64_t now_us) const;

  bool armed() const { return armed_; }
  bool pressing() const { return pressing_; }
  ConfirmSource source() const { return source_; }
  const ConfirmTarget& target() const { return target_; }

 private:
  ConfirmTarget target_;
  ConfirmSource source_ = ConfirmSource::None;
  int64_t armed_us_ = 0;
  int64_t deadline_us_ = 0;
  int64_t press_us_ = 0;
  int64_t release_us_ = 0;
  bool armed_ = false;
  bool pressing_ = false;
  bool released_ = false;
};

}  // namespace panel
