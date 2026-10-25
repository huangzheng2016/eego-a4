// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// panel_types —— 面板应用共享枚举（纯类型，无 ESP-IDF 依赖）。
// 危险动作首次激活只布防；同一控件的一次全新持续按住才执行，其他输入取消。

#include <cstdint>

namespace panel {

// 设备屏幕。主页 = Dashboard（已配置）或 Onboarding（未配置：引导到 web 配置）。
enum class Page : uint8_t {
  Dashboard = 0,
  Onboarding,
  AlertDetail,  // 告警详情（从告警面板/横幅进入；↑/↓ 浏览同一列表）
  Status,
  Settings,
  Help,
  Provision,
};

constexpr Page homePage(bool dashboard_loaded) {
  return dashboard_loaded ? Page::Dashboard : Page::Onboarding;
}

constexpr bool isHomePage(Page page) {
  return page == Page::Dashboard || page == Page::Onboarding;
}

constexpr bool dashboardUsesRuntimeStatus(Page current) { return current == Page::Dashboard; }

// S4 设置行标识（显示顺序 = 枚举顺序）。可见行列表由 PanelApp::visibleSettings()
// 按当前能力（如前光是否可用）过滤生成；行为/取值一律按 id 分发，不依赖行号。
enum class SettingId : uint8_t {
  Network = 0,
  RecoveryAp,
  WifiReset,
  Bluetooth,
  Language,
  Timezone,
  Frontlight,
  Brightness,
  Warmth,
  DataRefresh,
  PollNow,
  EpdFull,
  EpdNow,
  IdleAfter,    // 空闲判定：无交互多少分钟后进入空闲刷新节奏（0 = 关闭）
  IdleRefresh,  // 空闲刷新间隔：空闲时按本地墙钟整 N 分钟边界合并刷新
  PowerMode,
  BatteryPoll,
  IdleSleep,
  SleepPlan,
  NextWake,
  LastWake,
  Storage,
  Diagnostics,
  RunDiagnostics,
  Security,
  Mutes,
  FactoryReset,
  Version,
  InputHelp,
  InputReset,
  BleObserver,
  BleUnbind,
  Count,
};

// 5 秒模态窗口；恢复动作只能由实体 Power 的全新持续按住完成。
enum class ConfirmKind : uint8_t {
  None = 0,
  Mute,
  Clear,
  ClearMutes,
  NetworkRecovery,
  WifiReset,
  FactoryReset,
  BleProvision,
  BleUnbind,
  DashboardAction,
};

constexpr bool requiresPhysicalConfirm(ConfirmKind kind) {
  return kind == ConfirmKind::NetworkRecovery || kind == ConfirmKind::WifiReset ||
         kind == ConfirmKind::FactoryReset || kind == ConfirmKind::BleProvision ||
         kind == ConfirmKind::BleUnbind;
}

// 电池模式阶段机。
enum class BattPhase : uint8_t {
  Mains = 0,  // 非常供电模式（FSM 不动作）
  Polling,    // 重连 + 等待连接/超时 → 触发全源轮询
  Serving,    // 交互窗口（输入延长，到点休眠）
};

}  // namespace panel
