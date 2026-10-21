// SPDX-License-Identifier: GPL-3.0-or-later
#include "input_navigation.h"

#include <algorithm>

namespace panel {

void NavigationCoalescer::cancel() {
  kind_ = NavigationKind::None;
  delta_ = 0;
  page_epoch_ = 0;
  latest_us_ = 0;
}

int NavigationCoalescer::submit(NavigationKind kind, int delta, bool busy,
                                uint64_t page_epoch, int64_t now_us) {
  if (kind == NavigationKind::None || delta == 0) return 0;
  if (!busy) {
    cancel();
    return delta;
  }
  if (kind_ != kind || page_epoch_ != page_epoch) {
    kind_ = kind;
    delta_ = 0;
  }
  page_epoch_ = page_epoch;
  latest_us_ = now_us;
  delta_ = std::max(-6, std::min(6, delta_ + delta));
  return 0;
}

int NavigationCoalescer::flush(bool busy, uint64_t page_epoch, int64_t now_us,
                               NavigationKind& kind) {
  kind = NavigationKind::None;
  if (kind_ == NavigationKind::None || busy) return 0;
  if (page_epoch_ != page_epoch || now_us - latest_us_ > kMaxAgeUs) {
    cancel();
    return 0;
  }
  const int delta = delta_;
  kind = kind_;
  cancel();
  return delta;
}

}  // namespace panel
