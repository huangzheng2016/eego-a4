// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace panel {
enum class Page : uint8_t {
  Dashboard = 0,
  Onboarding,
  AlertDetail,
  Status,
  Settings,
  Help,
  Provision,
  BlePair,
};

constexpr Page homePage(bool dashboard_loaded) {
  return dashboard_loaded ? Page::Dashboard : Page::Onboarding;
}

constexpr bool isHomePage(Page page) {
  return page == Page::Dashboard || page == Page::Onboarding;
}

constexpr bool dashboardUsesRuntimeStatus(Page current) { return current == Page::Dashboard; }

enum class SettingId : uint8_t {
  KeepAwake = 0,
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
  IdleAfter,
  IdleRefresh,
  ScreenRotate,
  PowerMode,
  ChargeSignal,
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
  TouchCal,
  TouchMarks,
  BleObserver,
  BleUnbind,
  Count,
};

enum class ConfirmKind : uint8_t {
  None = 0,
  Mute,
  Clear,
  ClearMutes,
  NetworkRecovery,
  WifiReset,
  FactoryReset,
  BleUnbind,
  DashboardAction,
  Shutdown,
  Sleep,
};

constexpr bool requiresPhysicalConfirm(ConfirmKind kind) {
  return kind == ConfirmKind::NetworkRecovery || kind == ConfirmKind::WifiReset ||
         kind == ConfirmKind::FactoryReset ||
         kind == ConfirmKind::BleUnbind || kind == ConfirmKind::Shutdown ||
         kind == ConfirmKind::Sleep;
}

enum class BattPhase : uint8_t {
  Mains = 0,
  Fetching,
  Interactive,
};

}  // namespace panel
