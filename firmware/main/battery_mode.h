// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

#include "battery_policy.h"

namespace panel::battery {
enum class WakeSource : uint8_t {
  PowerOn = 0,
  Timer,
  PowerKey,     // EXT0
  Buttons,
  Other,
};

WakeSource wakeSource();
const char* wakeSourceName(WakeSource source);
inline bool interactiveWake(WakeSource s) {
  return s == WakeSource::PowerKey || s == WakeSource::Buttons;
}
inline bool deepSleepWake(WakeSource s) { return s != WakeSource::PowerOn; }

RtcState& rtcState();
void commitRtcState();

inline constexpr const char* kFramePath = "/sdcard/alertpanel/battery_frame.bin";
bool loadStoredFrame(uint8_t* out, size_t size, uint64_t expected_hash);
bool storeFrame(const uint8_t* frame, size_t size, uint64_t hash);

[[noreturn]] void deepSleep(uint64_t sleep_s, bool buttons);

[[noreturn]] void powerOff();

}  // namespace panel::battery
