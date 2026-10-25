// SPDX-License-Identifier: GPL-3.0-or-later
// Host render-harness stub: monotonic microseconds.
#pragma once
#include <chrono>
#include <cstdint>
inline int64_t esp_timer_get_time() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}
