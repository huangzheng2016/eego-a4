// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

//
//

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace eego {
class PortraitCanvas;
}

namespace panel {
enum class DeviceState : uint8_t {
  Running = 0,
  KeepAwake,
  Idle,
  Awake,
  Sleeping,
  NightPause,
};

struct DeviceStateInfo {
  DeviceState state = DeviceState::Running;
  uint32_t keep_awake_min = 0;
  uint64_t next_unix = 0;
  uint32_t idle_refresh_min = 5;
  uint32_t awake_timeout_s = 0;

  bool operator==(const DeviceStateInfo& o) const {
    return state == o.state && keep_awake_min == o.keep_awake_min && next_unix == o.next_unix &&
           idle_refresh_min == o.idle_refresh_min && awake_timeout_s == o.awake_timeout_s;
  }
  bool operator!=(const DeviceStateInfo& o) const { return !(*this == o); }
};

std::string deviceStateLabel(const DeviceStateInfo& info, int tz_offset_min);
std::vector<std::string> deviceStateLabels(const DeviceStateInfo& info, int tz_offset_min);
// "更新于 HH:MM"; with a valid now_unix, stamps from another day get the date.
std::string updatedLabel(uint64_t unix_s, int tz_offset_min, uint64_t now_unix = 0);
enum class PowerDetail : uint8_t { None = 0, Forced, UsbHost, Charging, Full };
struct PowerStatusInput {
  bool external = true;
  PowerDetail detail = PowerDetail::None;
  bool charging = false;
  uint16_t battery_mv = 0;
  uint8_t battery_pct = 0;
  DeviceStateInfo state;
  bool auto_sleep = false;
  uint32_t sleep_timeout_s = 0;
};
std::vector<std::pair<std::string, std::string>> powerStatusRows(const PowerStatusInput& in,
                                                                 int tz_offset_min);
const char* deviceStateApiName(DeviceState state);

void drawStateIcon(eego::PortraitCanvas& c, int16_t x, int16_t y, int16_t size, DeviceState state);
int16_t stateIconSize(int16_t font_h);

struct StatusLineContent {
  DeviceState icon = DeviceState::Running;
  bool show_icon = true;
  std::string left;
  std::string right;
  bool left_bold = false;
  std::vector<std::string> left_alt;
  std::vector<std::string> right_alt;
  bool pager = false;
};
int16_t statusLineScale(int16_t h);
void drawStatusLine(eego::PortraitCanvas& c, int16_t x, int16_t y, int16_t w, int16_t h,
                    const StatusLineContent& content);

}  // namespace panel
