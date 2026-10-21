// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>

namespace eego::net {

constexpr uint32_t effectivePollIntervalMs(uint32_t override_interval_ms,
                                           uint32_t source_interval_ms) {
  return override_interval_ms == 0 ? source_interval_ms : override_interval_ms;
}

constexpr int64_t dueAfterRefreshChange(uint32_t override_interval_ms,
                                        uint32_t source_interval_ms, int64_t now_ms) {
  return now_ms + effectivePollIntervalMs(override_interval_ms, source_interval_ms);
}

inline uint32_t pollBackoffMs(uint32_t override_interval_ms, uint32_t source_interval_ms,
                              uint32_t level, uint32_t maximum_ms) {
  const uint64_t base = effectivePollIntervalMs(override_interval_ms, source_interval_ms);
  const uint64_t backoff = level >= 32 ? UINT64_MAX : (base << level);
  return static_cast<uint32_t>(std::min<uint64_t>(backoff, maximum_ms));
}

}  // namespace eego::net
