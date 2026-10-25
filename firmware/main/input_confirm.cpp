// SPDX-License-Identifier: GPL-3.0-or-later
#include "input_confirm.h"

namespace panel {

void FreshPressConfirm::arm(const ConfirmTarget& target, int64_t now_us) {
  target_ = target;
  source_ = ConfirmSource::None;
  armed_us_ = now_us;
  deadline_us_ = now_us + kConfirmWindowUs;
  press_us_ = release_us_ = 0;
  armed_ = true;
  pressing_ = false;
  released_ = false;
}

void FreshPressConfirm::cancel() {
  target_ = ConfirmTarget{};
  source_ = ConfirmSource::None;
  armed_us_ = deadline_us_ = press_us_ = release_us_ = 0;
  armed_ = false;
  pressing_ = false;
  released_ = false;
}

bool FreshPressConfirm::expired(int64_t now_us) const {
  return armed_ && now_us > deadline_us_;
}

bool FreshPressConfirm::beginPress(ConfirmSource source, const std::string& control,
                                   int64_t press_us, int64_t now_us) {
  if (!armed_ || pressing_ || source == ConfirmSource::None || expired(now_us) ||
      press_us <= armed_us_ || control.empty() || control != target_.control) {
    return false;
  }
  if (target_.physical_power_only && source != ConfirmSource::Power) return false;
  source_ = source;
  press_us_ = press_us;
  release_us_ = 0;
  pressing_ = true;
  released_ = false;
  return true;
}

void FreshPressConfirm::noteRelease(ConfirmSource source, int64_t release_us) {
  if (!armed_ || !pressing_ || released_ || source != source_) return;
  release_us_ = release_us;
  released_ = true;
}

bool FreshPressConfirm::completePress(ConfirmSource source, int64_t now_us) const {
  return armed_ && pressing_ && released_ && source_ == source && !expired(now_us) &&
         release_us_ >= press_us_ && release_us_ - press_us_ < eego::kInputLongPressUs;
}

bool confirmRecordTargetCurrent(const ConfirmTarget& target, ConfirmRecordRule rule,
                                const alert_core::AlertSnapshot* snapshot) {
  if (snapshot == nullptr || snapshot->seq != target.snapshot_epoch) return false;
  for (const auto& record : snapshot->records) {
    if (record.id != target.record_id) continue;
    if (rule == ConfirmRecordRule::Mute) return !record.muted;
    return record.state == alert_core::AlertState::Active ||
           record.state == alert_core::AlertState::Acked;
  }
  return false;
}

}  // namespace panel
