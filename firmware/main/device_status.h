// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// device_status —— 设备状态「一眼可见」：状态标签（运行中 / 保持唤醒 / 空闲 / 已唤醒 /
// 休眠中 / 夜间暂停）、配套矢量图标与 dashboard 底部状态行的统一绘制。
//
//   * 顶部状态栏（panel_render drawStatusBar）与 dashboard 底部状态行
//     （dashboard_runtime drawStatus、深睡帧）共用同一套标签与图标；
//   * 文字为正文字号（UI 字体 2x）；状态行另在右侧常驻「更新于 HH:MM」
//     （本帧实际刷屏的时刻），屏幕是否陈旧一目了然；
//   * 标签只依赖低频变化的输入（剩余分钟按 keep_awake::displayMinutes 粗化、空闲
//     的「下次」= 上次刷新后的下一个边界、休眠的「下次」= 保证刷新的最迟时刻），
//     状态行不会因为倒计时逼出高频刷屏。
//
// 本文件只做「状态 → 文字/像素」，不持有业务状态；主机渲染工具链（render.sh）
// 直接链接本文件生成状态行/状态页 PNG。

#include <cstdint>
#include <string>
#include <vector>

namespace eego {
class PortraitCanvas;
}

namespace panel {

enum class DeviceState : uint8_t {
  Running = 0,  // 常供电、交互活跃：屏幕即时更新
  KeepAwake,    // 保持唤醒（配置模式）
  Idle,         // 空闲刷新节奏：屏幕更新合并到墙钟边界
  Awake,        // 电池供电、按键唤醒后的交互期：无操作超时后深睡
  Sleeping,     // 电池深睡帧（最后一次刷屏）
  NightPause,   // 夜间暂停深睡帧
};

struct DeviceStateInfo {
  DeviceState state = DeviceState::Running;
  uint32_t keep_awake_min = 0;    // KeepAwake：显示的剩余分钟（已粗化）
  uint64_t next_unix = 0;         // Idle：下一次刷新边界；Sleeping：最迟刷新；NightPause：恢复
  uint32_t idle_refresh_min = 5;  // Idle 且时间不可用：刷新间隔
  uint32_t awake_timeout_s = 0;   // Awake：无操作多少秒后深睡

  bool operator==(const DeviceStateInfo& o) const {
    return state == o.state && keep_awake_min == o.keep_awake_min && next_unix == o.next_unix &&
           idle_refresh_min == o.idle_refresh_min && awake_timeout_s == o.awake_timeout_s;
  }
  bool operator!=(const DeviceStateInfo& o) const { return !(*this == o); }
};

// 当前语言的状态标签，例如「保持唤醒 · 剩余 25 分」「空闲 · 下次 14:35」。
std::string deviceStateLabel(const DeviceStateInfo& info, int tz_offset_min);
// 由长到短的候选标签（首项 = deviceStateLabel）：空间不足时依次退到更短的写法，
// 例如「休眠中 · 下次 10:55 · 按键唤醒」→「休眠中 · 下次 10:55」→「休眠中」。
std::vector<std::string> deviceStateLabels(const DeviceStateInfo& info, int tz_offset_min);
// 「更新于 HH:MM」（unix < 2000 年 = 时间不可用 → 「更新于 --:--」）。
std::string updatedLabel(uint64_t unix_s, int tz_offset_min);
// API 名：active | keep_awake | idle | awake | sleeping | night_pause。
const char* deviceStateApiName(DeviceState state);

// 状态图标（size×size 方框内，黑色）：运行中=实心圆点、保持唤醒=太阳、空闲=时钟、
// 已唤醒=圆环、休眠/夜间暂停=月牙。
void drawStateIcon(eego::PortraitCanvas& c, int16_t x, int16_t y, int16_t size, DeviceState state);
// 与字号匹配的图标边长（≈ 大写字高 × 1.25，最小 12）。
int16_t stateIconSize(int16_t font_h);

// dashboard 底部状态行：顶部 1px 分隔线 + [图标][左侧文字……][右侧文字]。
// 左侧放不下时省略号截断；右侧（更新时刻/页码）优先完整显示。
struct StatusLineContent {
  DeviceState icon = DeviceState::Running;
  bool show_icon = true;
  std::string left;
  std::string right;
  bool left_bold = false;
  // 可选的更短写法（由长到短）。绘制时按 左候选（外层）× 右候选（内层）的顺序取第一
  // 个完整放得下的组合；都放不下则用最短的一对并对左侧省略号截断。
  std::vector<std::string> left_alt;
  std::vector<std::string> right_alt;
};
// 正文字号（2x）放得下 h-3 时用 2x，否则回退 1x。
int16_t statusLineScale(int16_t h);
void drawStatusLine(eego::PortraitCanvas& c, int16_t x, int16_t y, int16_t w, int16_t h,
                    const StatusLineContent& content);

}  // namespace panel
