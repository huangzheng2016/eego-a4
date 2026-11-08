// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

//
//
//

#include <cstdint>

namespace panel {
class TimeSource {
 public:
  static uint64_t now();

  static bool valid();

  static bool syncSystemFromRtc();

  static bool writeRtc(uint64_t unix_now);
};

}  // namespace panel
