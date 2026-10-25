// SPDX-License-Identifier: GPL-3.0-or-later
// 远程切屏请求与当前屏状态（契约见 admin/device_screen.h）。
#include "admin/device_screen.h"

#include <atomic>
#include <mutex>

namespace eego::admin {

namespace {

std::mutex g_mutex;
ScreenState g_state;
std::atomic<int32_t> g_request{-1};

}  // namespace

void publishScreenState(const ScreenState& state) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_state = state;
}

ScreenState screenState() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_state;
}

void requestScreen(uint32_t index) {
  g_request.store(static_cast<int32_t>(index), std::memory_order_release);
}

bool consumeScreenRequest(uint32_t& index) {
  const int32_t value = g_request.exchange(-1, std::memory_order_acq_rel);
  if (value < 0) return false;
  index = static_cast<uint32_t>(value);
  return true;
}

}  // namespace eego::admin
