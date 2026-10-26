// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// panel_types —— 面板应用共享枚举（纯类型，无 ESP-IDF 依赖）。
// 危险动作首次激活只布防；同一控件全新的一次短按才执行，其他输入取消。

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
  KeepAwake = 0,  // 保持唤醒 N 分钟（动作行：开启 / 剩余不足一半时延长 / 否则取消）
  Network,
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
  ScreenRotate, // 多屏自动轮播间隔（关/1/2/5/10/30 分钟）
  // 电池组：供电模式 → 电池唤醒间隔 → 最长不刷屏 → 交互超时 → 夜间暂停 → 下次唤醒/唤醒方式
  PowerMode,
  ChargeSignal,  // 充电信号极性（自动 / 高有效 / 低有效）：充电与电池显示相反时切换
  BatteryRefresh,
  BatteryMaxStale,
  BatteryInteractive,
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
  TouchCal,     // 触摸校准（全屏五点校准）
  TouchMarks,   // 显示触点（调试：点按处画标记；运行时开关，默认关）
  BleObserver,
  BleUnbind,
  Count,
};

// 5 秒模态窗口；恢复动作只能由实体 Power 再按一次完成。
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
  Shutdown,  // Power 长按布防，再按一次 Power 关机
};

constexpr bool requiresPhysicalConfirm(ConfirmKind kind) {
  return kind == ConfirmKind::NetworkRecovery || kind == ConfirmKind::WifiReset ||
         kind == ConfirmKind::FactoryReset || kind == ConfirmKind::BleProvision ||
         kind == ConfirmKind::BleUnbind || kind == ConfirmKind::Shutdown;
}

// 电池模式阶段机（README「电池模式」）。
enum class BattPhase : uint8_t {
  Mains = 0,    // 常供电（不深睡；夜间暂停除外）
  Fetching,     // 定时唤醒快速路径：WiFi 快连 → 一批取数（硬预算）→ 渲染/判定 → 深睡
  Interactive,  // 按键/上电唤醒：完整应用（WiFi、管理 API）；无操作超时后深睡
};

}  // namespace panel
