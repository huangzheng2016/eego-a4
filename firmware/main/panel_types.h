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
  Dashboard,
};

constexpr Page dashboardStartupPage(Page current, bool dashboard_loaded) {
  return dashboard_loaded && current != Page::Provision ? Page::Dashboard : current;
}

constexpr bool dashboardUsesRuntimeStatus(Page current) { return current == Page::Dashboard; }

// 二次确认状态（5s 窗口；恢复动作只能由实体电源键完成第二次确认）。
enum class ConfirmKind : uint8_t {
  None = 0,
  AckAll,
  Mute,
  Clear,
  ClearMutes,
  NetworkRecovery,
  WifiReset,
  FactoryReset,
};

constexpr bool requiresPhysicalConfirm(ConfirmKind kind) {
  return kind == ConfirmKind::NetworkRecovery || kind == ConfirmKind::WifiReset ||
         kind == ConfirmKind::FactoryReset;
}

// 电池模式阶段机。
enum class BattPhase : uint8_t {
  Mains = 0,  // 非常供电模式（FSM 不动作）
  Polling,    // 重连 + 等待连接/超时 → 触发全源轮询
  Serving,    // 交互窗口（输入延长，到点休眠）
};

// 触控只可确认普通操作；恢复动作的第二次确认不接受触屏。
constexpr bool confirmTargetMatches(ConfirmKind kind, Page page, int hitIdx, bool hitRow) {
  if (requiresPhysicalConfirm(kind)) return false;
  switch (kind) {
    case ConfirmKind::AckAll:
      return page == Page::Overview && !hitRow && hitIdx == 1;
    case ConfirmKind::Mute:
      return page == Page::Detail && !hitRow && hitIdx == 1;
    case ConfirmKind::Clear:
      return page == Page::Detail && !hitRow && hitIdx == 2;
    case ConfirmKind::ClearMutes:
      return page == Page::Settings && hitRow && hitIdx == 24;
    default:
      return false;
  }
}

}  // namespace panel
