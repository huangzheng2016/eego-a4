// SPDX-License-Identifier: GPL-3.0-or-later
#include "input_confirm.h"

namespace panel {

void FreshHoldConfirm::arm(const ConfirmTarget& target, int64_t now_us) {
  target_ = target;
  source_ = ConfirmSource::None;
  armed_us_ = now_us;
  deadline_us_ = now_us + kConfirmWindowUs;
  hold_start_us_ = 0;
  armed_ = true;
  holding_ = false;
}

void FreshHoldConfirm::cancel() {
  target_ = ConfirmTarget{};
  source_ = ConfirmSource::None;
  armed_us_ = deadline_us_ = hold_start_us_ = 0;
  armed_ = false;
  holding_ = false;
}

bool FreshHoldConfirm::expired(int64_t now_us) const {
  return armed_ && now_us > deadline_us_;
}

bool FreshHoldConfirm::beginHold(ConfirmSource source, const std::string& control,
                                 int64_t press_us, int64_t now_us) {
  if (!armed_ || holding_ || expired(now_us) || press_us <= armed_us_ ||
      control.empty() || control != target_.control) {
    return false;
  }
  if (target_.physical_power_only && source != ConfirmSource::Power) return false;
  source_ = source;
  hold_start_us_ = press_us;
  holding_ = true;
  return true;
}

bool FreshHoldConfirm::completeHold(ConfirmSource source, int64_t event_us,
                                    int64_t now_us) const {
  return armed_ && holding_ && source_ == source && !expired(now_us) &&
         event_us >= hold_start_us_ + eego::kInputLongPressUs;
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
