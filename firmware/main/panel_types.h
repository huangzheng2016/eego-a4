// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// panel_types —— 面板应用共享枚举与二次确认命中判定（纯函数，无 ESP-IDF 依赖，
// 可宿主编译测试）。
//
// 二次确认契约（panel_app.h 同述）：危险/批量动作（全部确认/静音/清除/清静音）
// 首次激活布防（5s 窗口），同一控件的再次激活执行，任何其他输入取消。
// 命中编码与 activateSoftKey 对齐：软键下标（S4 软键 0=返回）；
// S4 行 hitRow=true 且 hitIdx=行下标+1；内容命中 hitIdx=-1（永不匹配 → 取消）。

#include <cstdint>

namespace panel {

enum class Page : uint8_t {
  Overview = 0,
  Table,
  Detail,
  Status,
  Settings,
  Provision,
};

// 二次确认状态（5s 窗口内再次激活才执行；任何其他输入取消）。
enum class ConfirmKind : uint8_t {
  None = 0,
  AckAll,
  Mute,
  Clear,
  ClearMutes,
};

// 电池模式阶段机。
enum class BattPhase : uint8_t {
  Mains = 0,  // 非常供电模式（FSM 不动作）
  Polling,    // 重连 + 等待连接/超时 → 触发全源轮询
  Serving,    // 交互窗口（输入延长，到点休眠）
};

// 命中目标是否即布防确认的控件（同控件再次激活 = 执行；其他输入 = 取消）。
constexpr bool confirmTargetMatches(ConfirmKind kind, Page page, int hitIdx, bool hitRow) {
  switch (kind) {
    case ConfirmKind::AckAll:     // S0 软键 1 [全部确认]
      return page == Page::Overview && !hitRow && hitIdx == 1;
    case ConfirmKind::Mute:       // S2 软键 1 [静音]
      return page == Page::Detail && !hitRow && hitIdx == 1;
    case ConfirmKind::Clear:      // S2 软键 2 [清除]
      return page == Page::Detail && !hitRow && hitIdx == 2;
    case ConfirmKind::ClearMutes:  // S4 第 5 行（hitIdx = 行下标 4 + 1）
      return page == Page::Settings && hitRow && hitIdx == 5;
    default:
      return false;
  }
}

}  // namespace panel
