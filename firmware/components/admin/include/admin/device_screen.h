// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// device_screen —— 远程切换 dashboard 当前屏（POST /api/v1/device/screen）与
// 当前屏状态（GET /api/v1/status 的 "screen"）。
//
// 每个可见 dashboard 页 = 一屏（最多 8 屏）。app_task 定期发布当前屏序号/屏数；
// httpd 按发布的状态把 {"index":n} / {"step":±1} 解析成目标屏序号并登记，app_task
// 取走后切屏（视为一次交互，立即显示）。
//
// 本头的 inline 纯函数无 ESP-IDF 依赖（主机测试直接 include）。

#include <cstdint>
#include <string>

namespace eego::admin {

namespace device_screen {

inline constexpr int64_t kMaxScreens = 8;

// 目标屏序号（0 起）；-1 = 无效（无屏、index 越界、step 非 ±1..±kMaxScreens）。
// step 循环：最后一屏 +1 → 第一屏。
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

// app_task 发布的当前屏状态。
struct ScreenState {
  bool available = false;  // dashboard 已加载
  uint32_t index = 0;      // 当前屏（0 起）
  uint32_t count = 0;      // 屏数（可见页数）
  std::string page_id;     // 当前屏对应的页 id
  bool shown = false;      // dashboard 当前是否在屏上（否则在设置/状态等页面）
  uint32_t rotate_min = 0; // 自动轮播间隔（0 = 关）
};
void publishScreenState(const ScreenState& state);
ScreenState screenState();

// httpd → app_task：登记目标屏（覆盖未取走的旧请求）。
void requestScreen(uint32_t index);
// app_task：取走待处理请求。
bool consumeScreenRequest(uint32_t& index);

}  // namespace eego::admin
