// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

//
//
//

#include <cstdint>
#include <string>

namespace eego::admin {
namespace keep_awake {
inline constexpr uint32_t kDefaultMinutes = 30;
inline constexpr uint32_t kMaxMinutes = 240;
inline constexpr uint32_t kChoices[] = {10, 30, 60, 120};

inline bool validSettingMinutes(uint32_t minutes) {
  for (uint32_t choice : kChoices) {
    if (choice == minutes) return true;
  }
  return false;
}

inline bool validRequestMinutes(int64_t minutes) { return minutes >= 0 && minutes <= kMaxMinutes; }

inline int64_t deadlineMs(int64_t now_ms, uint32_t minutes) {
  if (minutes == 0) return 0;
  const int64_t until = now_ms + static_cast<int64_t>(minutes) * 60000;
  return until == 0 ? 1 : until;
}

inline bool active(int64_t now_ms, int64_t until_ms) { return until_ms != 0 && now_ms < until_ms; }

inline uint32_t remainingS(int64_t now_ms, int64_t until_ms) {
  if (!active(now_ms, until_ms)) return 0;
  return static_cast<uint32_t>((until_ms - now_ms + 999) / 1000);
}

inline uint32_t displayMinutes(uint32_t remaining_s) {
  if (remaining_s == 0) return 0;
  uint32_t minutes = (remaining_s + 59) / 60;
  if (minutes > 10) minutes = (minutes + 4) / 5 * 5;
  return minutes;
}

inline uint32_t toggleMinutes(uint32_t remaining_s, uint32_t duration_min) {
  if (duration_min == 0) duration_min = kDefaultMinutes;
  if (remaining_s == 0) return duration_min;
  if (remaining_s <= duration_min * 60 / 2) return duration_min;
  return 0;
}

}  // namespace keep_awake

int64_t keepAwakeUntilMs();
int64_t setKeepAwakeMinutes(uint32_t minutes);
uint32_t keepAwakeRevision();

struct PowerSnapshot {
  bool valid = false;
  bool mains = true;
  std::string source;          // "mains" | "battery"
  std::string source_reason;   // battery::powerReasonName
  std::string power_mode;
  std::string mode;            // active | keep_awake | idle | awake | sleeping
  int battery_pct = -1;
  int battery_mv = 0;
  bool charging = false;
  bool usb_host = false;
  uint64_t next_wake = 0;
  uint64_t next_refresh = 0;
  int64_t sleep_in_s = -1;
  std::string last_wake_reason;
  uint32_t sleeps_since_boot = 0;
  uint64_t last_screen_update = 0;
};
void publishPowerSnapshot(const PowerSnapshot& snapshot);
PowerSnapshot powerSnapshot();

}  // namespace eego::admin
