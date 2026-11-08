// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

//
//   Settings / Help / Provision
//
//
//
//

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "alert_core/snapshot.h"
#include "battery_mode.h"
#include "device_status.h"
#include "alert_core/type_registry.h"
#include "board/board_boot.h"
#include "dashboard_runtime.h"
#include "device_settings/device_settings.h"
#include "eego_hal.h"
#include "eego_ui.h"
#include "net_stack/net_status.h"
#include "panel_render.h"
#include "panel_types.h"
#include "refresh_policy.h"
#include "ui_text.h"
#include "admin/input_binding_model.h"
#include "ble/ble_manager.h"
#include "admin/render_preview.h"
#include "input_confirm.h"
#include "input_navigation.h"
#include "touch_cal_screen.h"

namespace alert_core {
struct MuteRule;
}  // namespace alert_core

namespace panel {
class PanelApp {
 public:
  struct Deps {
    eego::Uc8279cDisplay* display = nullptr;
    eego::Gslx680Touch* touch = nullptr;
    eego::PortraitCanvas* canvas = nullptr;
    eego::board::BootStatus boot;
    alert_core::SnapshotPublisher* publisher = nullptr;
    alert_core::TypeRegistry* registry = nullptr;
    bool fastWake = false;
  };

  PanelApp() = default;
  PanelApp(const PanelApp&) = delete;
  PanelApp& operator=(const PanelApp&) = delete;

  void begin(const Deps& deps);

  void service();

  void handleTouch(const eego::TouchEvent& ev);
  void handleButton(const eego::ButtonEvent& ev);
  void noteInputResume();

  bool renderDashboardPreview(const eego::admin::RenderPreviewRequest& request, uint8_t* rows,
                              size_t stride, eego::admin::RenderPreviewResult& result);

  void notifyNetEvent(int wifiState);
  void notifyAdminEvent(int type, int reason, uint32_t remainingS);

  void diagStatus();
  void diagBattery();
  void diagRtc();
  void diagSd();
  void diagNet();
  void diagAlerts();
  void diagInject(const char* json);
  void diagAckAll();
  void diagSmoke();
  void diagVersion();
  void diagLang(const char* arg);
  void diagPage(const char* arg);
  void diagDashboard(const char* arg);
  void diagTouch(const char* arg);

  void enterProvision();
  void leaveProvision();

 private:
  void onTouchTap(int16_t x, int16_t y);
  void dispatchTrigger(eego::admin::InputTrigger trigger, int64_t now_us);
  void executeBinding(const eego::admin::InputBinding& binding, int64_t now_us);
  void executeBuiltin(const std::string& name, int64_t now_us);

  Page home() const { return homePage(dashboard_.available()); }
  void gotoPage(Page p);
  void goHome();
  void serviceScreens(int64_t nowMs);
  void rotateForSleepFrame(uint64_t slotUnix);
  void back();
  void frontBack();
  void sleepByPowerKey(int64_t nowMs);
  void armShutdown();
  [[noreturn]] void shutdownNow();
  void moveFocus(int delta);
  void movePage(int delta);
  void requestNavigation(NavigationKind kind, int delta, int64_t now_us);
  void flushNavigation(int64_t now_us);
  bool inputBusy() const;
  uint64_t inputEpoch() const;
  std::string pageContext() const;
  void activateFocused();
  void activateSoftKey(size_t idx);

  std::vector<SettingId> visibleSettings() const;
  static std::vector<SettingId> visibleSettingsFor(bool frontlight);
  bool frontlightRowsVisible_ = false;
  void activateSettingRow(SettingId id);
  bool adjustSettingRow(SettingId id, int dir, bool coarse = false, bool wrap = false);
  static bool settingAdjustable(SettingId id);
  size_t settingsPage() const;
  bool pageEdge(int delta) const;
  size_t statusPageCount() const;

  struct TouchHit {
    enum class Kind : uint8_t { None, SoftKey, Row };
    Kind kind = Kind::None;
    int index = -1;
    bool valueLeft = false;
    bool valueRight = false;
  };
  TouchHit resolveTouchHit(int16_t x, int16_t y) const;
  std::string controlForHit(const TouchHit& hit) const;
  std::string focusedControl() const;
  std::string touchControl(int16_t x, int16_t y) const;
  void onTouchSwipe(eego::TouchEventType type, int64_t now_us);
  void onTouchHold(int16_t x, int16_t y);

  void serviceKeepAwake(int64_t nowMs);
  void toggleKeepAwake();
  int64_t keepAwakeUntil(int64_t nowMs) const;
  DeviceStateInfo computeDeviceState(int64_t nowMs, uint64_t refUnix);
  void syncDeviceState(int64_t nowMs, bool beforeRender);
  void publishPowerState(int64_t nowMs);
  PowerDetail powerDetail() const;
  battery::MainsDetector mainsDetector_;
  bool usbHost_ = false;
  uint32_t keepAwakeRevision_ = 0;
  bool keepAwakeWasActive_ = false;
  DeviceStateInfo deviceState_;
  int64_t lastDeviceStateMs_ = -100000;
  int64_t lastPowerPublishMs_ = -100000;
  uint64_t lastDisplayedUnix_ = 0;
  bool idleAnnounced_ = false;

  void syncDoublePress(bool force);

  void startTouchCalibration();
  bool serviceTouchCalibration(int64_t nowMs);
  void traceTouch(const eego::TouchEvent& ev);
  void noteTouchMark(int16_t x, int16_t y);
  void drawTouchMark(eego::PortraitCanvas& c) const;
  TouchCalScreen touchCal_;
  int64_t touchTraceUntilMs_ = 0;
  bool touchMarks_ = false;
  bool touchMarkPending_ = false;
  int16_t touchMarkX_ = -1;
  int16_t touchMarkY_ = -1;

  void doAck();
  void doClear(const std::string& id);
  void doMute(const alert_core::AlertRecord& rec);
  void doClearMutes();
  void persistMuteRules(const std::vector<alert_core::MuteRule>& rules);
  void refreshMuteRuleCount();
  void doPollNow();
  void doOpenRecovery();
  void doClearBleBonds();
  void serviceBlePairing(int64_t now);
  void leaveBlePair();
  void doWifiReset();
  void doFactoryReset();
  void resetInputBindings();
  bool updateSettings(const eego::settings::Settings& candidate);
  void applySettings(uint32_t changed);
  static void onSettingsChanged(const eego::settings::Settings& values, uint32_t changed,
                                void* ctx);
  uint64_t plannedWakeAt();
  battery::WakeInputs wakeInputs(uint64_t nowUnix) const;
  uint64_t currentSnapshotEpoch() const;
  void armConfirm(ConfirmKind kind);
  void armDashboardConfirm(const DashboardConfirmRequest& request);
  bool confirmTargetCurrent() const;
  void executeArmedConfirm();
  void clearConfirm();
  void showBanner(const char* text, uint32_t ms);
  void showBannerFmt(uint32_t ms, const char* fmt, ...)
      __attribute__((format(printf, 3, 4)));

  bool computeMains();
  void onModeChange(bool mains);
  void applyWifiPowerSave();
  bool dashboardLive() const { return mains_ || battPhase_ == BattPhase::Interactive; }
  void enterInteractive(int64_t nowMs);
  void batteryService(int64_t nowMs);
  void requestSleep(int64_t nowMs, const char* why);
  void serviceSleep(int64_t nowMs);
  [[noreturn]] void sleepNow();
  void presentSleepFrame(const battery::WakePlan& plan, uint64_t nowUnix);
  DeviceStateInfo sleepingState(uint64_t stamp, uint32_t intervalMin) const;
  void drawNotice(const char* title, const char* line1, const char* line2, const char* line3);
  uint64_t frameHash() const;
  bool batchSucceeded() const;

  struct NetIdleWait {
    bool active = false;
    int64_t deadlineMs = 0;
    int64_t nextProbeMs = 0;
    int64_t intervalMs = 0;
  };
  static void startNetIdleWait(NetIdleWait& w, int64_t nowMs, int64_t budgetMs);
  static int pollNetIdle(NetIdleWait& w, int64_t nowMs);

  void noteInteraction(int64_t nowMs);
  void configureRefreshPolicy();
  bool displayAllowed(int64_t nowMs);
  void noteDisplayed();

  void markDirty(bool forceFull = false) {
    dirty_ = true;
    if (forceFull) forceFull_ = true;
  }
  void render(int64_t nowMs);
  void buildStatusBar(StatusBarModel& m);
  void buildOnboarding(OnboardingModel& m);
  void buildDetail(AlertDetailModel& m);
  void buildStatus(KvModel& m);
  void buildS4(S4Model& m);
  void buildHelp(KvModel& m);

  void openDetail(const std::string& panel_id, const std::string& identity);
  void refreshDetailItems();
  const DashboardAlertItem* detailItem() const;
  const alert_core::AlertRecord* detailRecord() const;
  void moveDetail(int delta);
  std::string detailSignature() const;

  uint64_t nowUnix();
  void refreshBattery(int64_t nowMs);
  const eego::net::NetStatusSnapshot& netStatus();
  const alert_core::AlertRecord* findRecord(const std::string& id) const;
  void formatAge(uint64_t ts, char* out, size_t cap);
  void formatTs(uint64_t ts, char* out, size_t cap) const;
  const char* typeName(const std::string& typeId, std::string& buf) const;
  size_t softKeyCount() const;
  void softKeys(SoftKeyModel* out, size_t cap) const;
  int64_t nowMs() const;

  void drainAdminMailbox();
  void drainNetBits();
  void serviceApFallback(int64_t now);

  Deps deps_;
  DashboardRuntime dashboard_;
  eego::settings::Settings settings_;
  int settingsSubscription_ = -1;

  Page page_ = Page::Onboarding;
  Page helpReturn_ = Page::Settings;
  size_t focus_ = 0;
  bool softFocusMoved_ = false;
  bool adminActivePs_ = false;
  size_t statusPage_ = 0;
  size_t statusRows_ = 0;
  std::string detailPanel_;
  std::string detailIdentity_;
  size_t detailIndex_ = 0;
  std::vector<DashboardAlertItem> detailItems_;
  std::string detailRendered_;
  int64_t lastDetailCheckMs_ = 0;
  bool provisionSucceeded_ = false;
  uint64_t pageEpoch_ = 1;
  NavigationCoalescer navigation_;

  ConfirmKind confirm_ = ConfirmKind::None;
  FreshPressConfirm confirmState_;
  DashboardConfirmRequest confirmDashboard_;
  std::string activeInputControl_;
  bool suppressButtonShort_[3] = {false, false, false};
  bool suppressButtonLong_[3] = {false, false, false};
  bool suppressTouchTap_ = false;
  bool suppressTouchHold_ = false;
  bool suppressFrontShort_ = false;
  int64_t inputResumeUs_ = 0;
  bool touchGestureStale_ = false;
  bool frontGestureStale_ = false;
  bool buttonGestureStale_[3] = {false, false, false};
  bool inputEventStale(int64_t timestampUs, int64_t nowUs) const;
  std::string banner_;
  int64_t bannerDeadlineMs_ = 0;
  Page blePairReturn_ = Page::Dashboard;
  eego::ble::LinkState bleLinkSeen_ = eego::ble::LinkState::None;
  uint32_t blePairShownKey_ = 0;

  std::shared_ptr<const alert_core::AlertSnapshot> snapshot_;
  uint64_t snapshotSeq_ = 0;

  RefreshPolicy refreshPolicy_;
  uint64_t rotateEpoch_ = 0;
  bool rotateShown_ = true;
  int64_t lastScreenPublishMs_ = -100000;
  int64_t lastCriticalCheckMs_ = -100000;
  bool idleLogged_ = false;

  bool dirty_ = true;
  bool forceFull_ = true;
  int64_t lastRenderMs_ = -10000;
  int64_t lastFullRenderMs_ = 0;
  int64_t lastScheduleCheckMs_ = -10000;

  eego::BatteryInfo batCache_{};
  int64_t lastBatMs_ = -100000;
  bool charging_ = false;
  int64_t lastChargeMs_ = -100000;

  std::atomic<uint32_t> netBits_{0};
  std::atomic<int> wifiState_{0};
  void* adminQueue_ = nullptr;

  eego::Pcf8563Rtc rtc_{I2C_NUM_0};

  eego::net::NetStatusSnapshot netStatusCache_{};
  int64_t netStatusCacheMs_ = 0;

  size_t muteRuleCount_ = 0;

  int64_t renderedClockKey_ = -2;
  int64_t lastClockCheckMs_ = 0;
  bool dashboardWasAvailable_ = false;
  std::string onboardingKey_;
  int64_t lastOnboardingCheckMs_ = 0;
  int64_t lastStatusRenderMs_ = 0;

  std::string doublePressContext_;
  uint32_t doublePressRevision_ = 0;
  int64_t lastDoublePressSyncMs_ = -100000;
  bool doublePressSynced_ = false;

  NetIdleWait sleepIdleWait_;
  bool sleepPending_ = false;
  int64_t sleepRequestedMs_ = 0;

  bool mains_ = true;
  BattPhase battPhase_ = BattPhase::Mains;
  battery::WakeSource wakeSource_ = battery::WakeSource::PowerOn;
  bool fastWake_ = false;
  bool manualSleep_ = false;
  bool restartRequested_ = false;
  int64_t phaseStartMs_ = 0;
  bool batchStarted_ = false;
  bool wifiSeen_ = false;
  uint64_t wakeStartUnix_ = 0;
  bool displayedThisBoot_ = false;
  int64_t wakePressGuardUntilMs_ = 0;
  int64_t lastInputMs_ = 0;
  bool fallbackArmed_ = false;
  bool apFallbackAuto_ = false;
  int64_t apConnectedSinceMs_ = -1;
  int64_t apFallbackCheckMs_ = 0;
};

}  // namespace panel
