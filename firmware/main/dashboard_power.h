// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace panel {

inline bool dashboardPipelineReady(bool ready, const void* pipeline) {
  return ready && pipeline != nullptr;
}

inline bool dashboardReloadShouldSuspend(bool cycle_cancelled, bool mains, bool cycle_active,
                                         int64_t now_ms, int64_t deadline_ms) {
  return cycle_cancelled || (!mains && cycle_active && now_ms >= deadline_ms);
}

}  // namespace panel
