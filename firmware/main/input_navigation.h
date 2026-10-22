// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace panel {

enum class NavigationKind : uint8_t { None, Focus, Page };

class NavigationCoalescer {
 public:
  static constexpr int64_t kMaxAgeUs = 1200000;

  int submit(NavigationKind kind, int delta, bool busy, uint64_t page_epoch,
             int64_t now_us);
  int flush(bool busy, uint64_t page_epoch, int64_t now_us, NavigationKind& kind);
  void cancel();

  bool pending() const { return kind_ != NavigationKind::None; }

 private:
  NavigationKind kind_ = NavigationKind::None;
  int delta_ = 0;
  uint64_t page_epoch_ = 0;
  int64_t latest_us_ = 0;
};

}  // namespace panel
