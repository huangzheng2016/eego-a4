// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

//
//

#include <cstdint>
#include <string>

namespace eego::admin {
namespace device_screen {
inline constexpr int64_t kMaxScreens = 8;

inline int64_t resolveTarget(bool has_index, int64_t index, int64_t step, uint32_t current,
                             uint32_t count) {
  if (count == 0) return -1;
  if (has_index) return index >= 0 && index < static_cast<int64_t>(count) ? index : -1;
  if (step == 0 || step < -kMaxScreens || step > kMaxScreens) return -1;
  const int64_t n = static_cast<int64_t>(count);
  int64_t next = (static_cast<int64_t>(current % count) + step) % n;
  if (next < 0) next += n;
  return next;
}

}  // namespace device_screen

struct ScreenState {
  bool available = false;
  uint32_t index = 0;
  uint32_t count = 0;
  std::string page_id;
  bool shown = false;
  uint32_t rotate_min = 0;
};
void publishScreenState(const ScreenState& state);
ScreenState screenState();

void requestScreen(uint32_t index);
bool consumeScreenRequest(uint32_t& index);

}  // namespace eego::admin
