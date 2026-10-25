// SPDX-License-Identifier: GPL-3.0-or-later
// 设备状态标签、图标与状态行绘制（契约见 device_status.h）。
#include "device_status.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#include "battery_policy.h"
#include "eego_ui.h"
#include "panel_render.h"
#include "ui_text.h"

namespace panel {

namespace {

int16_t fontH(int16_t scale) {
  return static_cast<int16_t>(eego::FontManager::instance().uiFontHeight() * scale);
}

int16_t textW(const std::string& s, int16_t scale) {
  return static_cast<int16_t>(eego::FontManager::instance().uiTextWidth(s.c_str()) * scale);
}

bool clockValid(uint64_t unix_s) { return unix_s >= battery::kMinValidUnix; }

}  // namespace

std::string deviceStateLabel(const DeviceStateInfo& info, int tz) {
  char buf[96];
  int h = 0;
  int m = 0;
  switch (info.state) {
    case DeviceState::Running:
      return tr(Str::StateRunning);
    case DeviceState::KeepAwake:
      snprintf(buf, sizeof(buf), tr(Str::StateKeepAwakeFmt),
               static_cast<unsigned>(std::max<uint32_t>(1, info.keep_awake_min)));
      return buf;
    case DeviceState::Idle:
      if (!clockValid(info.next_unix)) {
        snprintf(buf, sizeof(buf), tr(Str::StateIdleNoClockFmt),
                 static_cast<unsigned>(info.idle_refresh_min));
        return buf;
      }
      battery::localHourMinute(info.next_unix, tz, h, m);
      snprintf(buf, sizeof(buf), tr(Str::StateIdleFmt), h, m);
      return buf;
    case DeviceState::Awake:
      snprintf(buf, sizeof(buf), tr(Str::StateAwakeFmt),
               static_cast<unsigned>(info.awake_timeout_s));
      return buf;
    case DeviceState::Sleeping:
      if (!clockValid(info.next_unix)) return tr(Str::StateSleeping);
      battery::localHourMinute(info.next_unix, tz, h, m);
      snprintf(buf, sizeof(buf), tr(Str::StateSleepingFmt), h, m);
      return buf;
    case DeviceState::NightPause:
      battery::localHourMinute(info.next_unix, tz, h, m);
      snprintf(buf, sizeof(buf), tr(Str::StateNightFmt), h, m);
      return buf;
  }
  return tr(Str::StateRunning);
}

std::vector<std::string> deviceStateLabels(const DeviceStateInfo& info, int tz) {
  std::vector<std::string> out{deviceStateLabel(info, tz)};
  char buf[96];
  int h = 0;
  int m = 0;
  switch (info.state) {
    case DeviceState::Running:
      break;
    case DeviceState::KeepAwake:
      snprintf(buf, sizeof(buf), tr(Str::StateKeepAwakeShortFmt),
               static_cast<unsigned>(std::max<uint32_t>(1, info.keep_awake_min)));
      out.emplace_back(buf);
      break;
    case DeviceState::Idle:
      if (clockValid(info.next_unix)) {
        battery::localHourMinute(info.next_unix, tz, h, m);
        snprintf(buf, sizeof(buf), tr(Str::StateIdleShortFmt), h, m);
        out.emplace_back(buf);
      }
      break;
    case DeviceState::Awake:
      out.emplace_back(tr(Str::StateAwake));
      break;
    case DeviceState::Sleeping:
      if (clockValid(info.next_unix)) {
        battery::localHourMinute(info.next_unix, tz, h, m);
        snprintf(buf, sizeof(buf), tr(Str::StateSleepingNextFmt), h, m);
        out.emplace_back(buf);
      }
      out.emplace_back(tr(Str::StateSleepingShort));
      break;
    case DeviceState::NightPause:
      battery::localHourMinute(info.next_unix, tz, h, m);
      snprintf(buf, sizeof(buf), tr(Str::NightPauseFmt), h, m);
      out.emplace_back(buf);
      out.emplace_back(tr(Str::StateNightShort));
      break;
  }
  return out;
}

std::string updatedLabel(uint64_t unix_s, int tz) {
  if (!clockValid(unix_s)) return tr(Str::UpdatedNoClock);
  int h = 0;
  int m = 0;
  battery::localHourMinute(unix_s, tz, h, m);
  char buf[48];
  snprintf(buf, sizeof(buf), tr(Str::UpdatedFmt), h, m);
  return buf;
}

std::vector<std::pair<std::string, std::string>> powerStatusRows(const PowerStatusInput& in,
                                                                 int tz) {
  std::vector<std::pair<std::string, std::string>> rows;
  char buf[96];
  const char* source = tr(in.external ? Str::ModeMains : Str::ModeBattery);
  const char* detail = nullptr;
  switch (in.detail) {
    case PowerDetail::Forced: detail = tr(Str::ReasonForced); break;
    case PowerDetail::UsbHost: detail = in.external ? tr(Str::ReasonUsbHost) : nullptr; break;
    case PowerDetail::Charging: detail = in.external ? tr(Str::ReasonCharging) : nullptr; break;
    case PowerDetail::Full: detail = in.external ? tr(Str::ReasonFullHold) : nullptr; break;
    case PowerDetail::None: break;
  }
  if (detail != nullptr) {
    snprintf(buf, sizeof(buf), tr(Str::PowerDetailFmt), source, detail);
    rows.emplace_back(tr(Str::LblPower), buf);
  } else {
    rows.emplace_back(tr(Str::LblPower), source);
  }
  if (in.battery_mv > 0) {
    snprintf(buf, sizeof(buf), "%u%% · %.2f V", static_cast<unsigned>(in.battery_pct),
             static_cast<double>(in.battery_mv) / 1000.0);
    std::string charge = buf;
    if (in.charging && !(detail != nullptr && in.detail == PowerDetail::Charging)) {
      charge += std::string(" · ") + tr(Str::Charging);
    }
    rows.emplace_back(tr(Str::LblCharge), charge);
  }
  // 已唤醒的长标签含「无操作 N 秒后休眠」，与自动休眠行重复 → 用短写法。
  rows.emplace_back(tr(Str::LblRunState), in.state.state == DeviceState::Awake
                                               ? std::string(tr(Str::StateAwake))
                                               : deviceStateLabel(in.state, tz));
  if (in.auto_sleep) {
    snprintf(buf, sizeof(buf), tr(Str::SleepInFmt), static_cast<unsigned>(in.sleep_timeout_s));
    rows.emplace_back(tr(Str::LblAutoSleep), buf);
  } else {
    rows.emplace_back(tr(Str::LblAutoSleep), tr(Str::SleepNever));
  }
  return rows;
}

const char* deviceStateApiName(DeviceState state) {
  switch (state) {
    case DeviceState::Running: return "active";
    case DeviceState::KeepAwake: return "keep_awake";
    case DeviceState::Idle: return "idle";
    case DeviceState::Awake: return "awake";
    case DeviceState::Sleeping: return "sleeping";
    case DeviceState::NightPause: return "night_pause";
  }
  return "active";
}

int16_t stateIconSize(int16_t font_h) {
  return std::max<int16_t>(12, static_cast<int16_t>(font_h * 5 / 9));
}

void drawStateIcon(eego::PortraitCanvas& c, int16_t x, int16_t y, int16_t s, DeviceState state) {
  const int16_t cx = static_cast<int16_t>(x + s / 2);
  const int16_t cy = static_cast<int16_t>(y + s / 2);
  const int16_t r = static_cast<int16_t>(s / 2);
  switch (state) {
    case DeviceState::Running:
      c.fillCircle(cx, cy, static_cast<int16_t>(r * 3 / 5), 0);
      break;
    case DeviceState::Awake:
      c.drawCircle(cx, cy, static_cast<int16_t>(r - 1), 0);
      c.drawCircle(cx, cy, static_cast<int16_t>(r - 2), 0);
      c.fillCircle(cx, cy, static_cast<int16_t>(std::max<int16_t>(2, r / 3)), 0);
      break;
    case DeviceState::KeepAwake: {
      // 太阳：实心圆盘 + 8 道 2px 光线。
      const int16_t core = static_cast<int16_t>(std::max<int16_t>(3, r * 2 / 5));
      c.fillCircle(cx, cy, core, 0);
      static constexpr float kDx[8] = {1.0f, 0.7071f, 0.0f, -0.7071f, -1.0f, -0.7071f, 0.0f, 0.7071f};
      static constexpr float kDy[8] = {0.0f, 0.7071f, 1.0f, 0.7071f, 0.0f, -0.7071f, -1.0f, -0.7071f};
      const float r0 = static_cast<float>(core) + 2.5f;
      const float r1 = static_cast<float>(r) - 0.5f;
      for (int i = 0; i < 8; ++i) {
        const int16_t x0 = static_cast<int16_t>(cx + kDx[i] * r0);
        const int16_t y0 = static_cast<int16_t>(cy + kDy[i] * r0);
        const int16_t x1 = static_cast<int16_t>(cx + kDx[i] * r1);
        const int16_t y1 = static_cast<int16_t>(cy + kDy[i] * r1);
        c.drawLine(x0, y0, x1, y1, 0);
        c.drawLine(static_cast<int16_t>(x0 + (kDy[i] != 0.0f ? 1 : 0)),
                   static_cast<int16_t>(y0 + (kDy[i] == 0.0f ? 1 : 0)),
                   static_cast<int16_t>(x1 + (kDy[i] != 0.0f ? 1 : 0)),
                   static_cast<int16_t>(y1 + (kDy[i] == 0.0f ? 1 : 0)), 0);
      }
      break;
    }
    case DeviceState::Idle: {
      // 时钟：2px 圆框 + 时针（向上）与分针（向右）。
      c.drawCircle(cx, cy, static_cast<int16_t>(r - 1), 0);
      c.drawCircle(cx, cy, static_cast<int16_t>(r - 2), 0);
      const int16_t hand = static_cast<int16_t>(r - 4);
      c.fillRect(cx, static_cast<int16_t>(cy - hand), 2, static_cast<int16_t>(hand + 1), 0);
      c.fillRect(cx, cy, static_cast<int16_t>(hand * 3 / 4 + 1), 2, 0);
      break;
    }
    case DeviceState::Sleeping:
    case DeviceState::NightPause: {
      // 月牙：实心圆减去右上偏移的白色圆。
      c.fillCircle(cx, cy, static_cast<int16_t>(r - 1), 0);
      c.fillCircle(static_cast<int16_t>(cx + r * 2 / 5), static_cast<int16_t>(cy - r * 3 / 10),
                   static_cast<int16_t>(r * 3 / 4), 1);
      break;
    }
  }
}

int16_t statusLineScale(int16_t h) { return fontH(2) <= h - 3 ? 2 : 1; }

void drawStatusLine(eego::PortraitCanvas& c, int16_t x, int16_t y, int16_t w, int16_t h,
                    const StatusLineContent& content) {
  c.fillRect(x, y, w, h, 1);
  c.drawLine(x, y, static_cast<int16_t>(x + w - 1), y, 0);
  const int16_t scale = statusLineScale(h);
  const int16_t th = fontH(scale);
  const int16_t band_y = static_cast<int16_t>(y + 1);
  const int16_t band_h = static_cast<int16_t>(h - 1);
  const int16_t ty = static_cast<int16_t>(band_y + std::max(0, (band_h - th) / 2));
  const int16_t pad = content.pager ? 22 : 12;
  const int16_t left_x = static_cast<int16_t>(x + pad);
  const int16_t right_edge = static_cast<int16_t>(x + w - pad);
  if (content.pager) {
    // 细 ‹ ›（2px 线宽）：虚拟翻屏键的提示，紧贴两端。
    const int16_t cy = static_cast<int16_t>(band_y + band_h / 2);
    const int16_t half = static_cast<int16_t>(std::max<int>(4, band_h / 5));
    for (int16_t d = 0; d < 2; ++d) {
      const int16_t lx = static_cast<int16_t>(x + 6 + d);
      const int16_t rx = static_cast<int16_t>(x + w - 7 - d);
      c.drawLine(static_cast<int16_t>(lx + half), static_cast<int16_t>(cy - half), lx, cy, 0);
      c.drawLine(lx, cy, static_cast<int16_t>(lx + half), static_cast<int16_t>(cy + half), 0);
      c.drawLine(static_cast<int16_t>(rx - half), static_cast<int16_t>(cy - half), rx, cy, 0);
      c.drawLine(rx, cy, static_cast<int16_t>(rx - half), static_cast<int16_t>(cy + half), 0);
    }
  }

  const int16_t icon = content.show_icon ? stateIconSize(th) : 0;
  const int16_t text_x = static_cast<int16_t>(left_x + (content.show_icon ? icon + 8 : 0));
  const int16_t max_right = static_cast<int16_t>((w - 2 * pad) * 3 / 5);
  std::vector<const std::string*> lefts{&content.left};
  for (const std::string& alt : content.left_alt) lefts.push_back(&alt);
  std::vector<const std::string*> rights{&content.right};
  for (const std::string& alt : content.right_alt) rights.push_back(&alt);
  const std::string* left = lefts.back();
  const std::string* right = rights.back();
  bool found = false;
  for (const std::string* l : lefts) {
    for (const std::string* r : rights) {
      const int16_t rw = r->empty() ? 0 : static_cast<int16_t>(textW(*r, scale) + 16);
      if (rw - 16 <= max_right &&
          text_x + textW(*l, scale) + (content.left_bold ? scale : 0) + rw <= right_edge) {
        left = l;
        right = r;
        found = true;
        break;
      }
    }
    if (found) break;
  }

  int16_t right_x = right_edge;
  if (!right->empty()) {
    const int16_t rw = std::min(textW(*right, scale), max_right);
    right_x = static_cast<int16_t>(right_edge - rw);
    drawTextFit(c, right_x, ty, rw, right->c_str(), scale, false, 0);
    right_x = static_cast<int16_t>(right_x - 16);
  }
  if (content.show_icon) {
    drawStateIcon(c, left_x, static_cast<int16_t>(band_y + (band_h - icon) / 2), icon, content.icon);
  }
  if (!left->empty() && right_x > text_x) {
    drawTextFit(c, text_x, ty, static_cast<int16_t>(right_x - text_x), left->c_str(), scale,
                content.left_bold, 0);
  }
}

}  // namespace panel
