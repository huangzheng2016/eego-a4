// SPDX-License-Identifier: GPL-3.0-or-later
// 保持唤醒覆盖与电源状态快照（契约见 admin/keep_awake.h）。
#include "admin/keep_awake.h"

#include <atomic>
#include <mutex>

#include "esp_timer.h"

namespace eego::admin {

namespace {

std::atomic<int64_t> g_until_ms{0};
std::atomic<uint32_t> g_revision{0};
std::mutex g_power_mutex;
PowerSnapshot g_power;

}  // namespace

int64_t keepAwakeUntilMs() { return g_until_ms.load(std::memory_order_acquire); }

int64_t setKeepAwakeMinutes(uint32_t minutes) {
  if (minutes > keep_awake::kMaxMinutes) minutes = keep_awake::kMaxMinutes;
  const int64_t until = keep_awake::deadlineMs(esp_timer_get_time() / 1000, minutes);
  g_until_ms.store(until, std::memory_order_release);
  g_revision.fetch_add(1, std::memory_order_acq_rel);
  return until;
}

uint32_t keepAwakeRevision() { return g_revision.load(std::memory_order_acquire); }

void publishPowerSnapshot(const PowerSnapshot& snapshot) {
  std::lock_guard<std::mutex> lock(g_power_mutex);
  g_power = snapshot;
  g_power.valid = true;
}

PowerSnapshot powerSnapshot() {
  std::lock_guard<std::mutex> lock(g_power_mutex);
  return g_power;
}

}  // namespace eego::admin
