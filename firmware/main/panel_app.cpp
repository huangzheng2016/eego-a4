// SPDX-License-Identifier: GPL-3.0-or-later
#include "panel_app.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "admin/admin.h"
#include "admin/admin_auth.h"
#include "admin/device_screen.h"
#include "admin/keep_awake.h"
#include "admin/provision_portal.h"
#include "dashboard_ui/preview_render.h"
#include "alert_core/types.h"
#include "ble/ble_manager.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "net_stack/uplink.h"
#include "net_stack/net_stack.h"
#include "net_stack/wifi_list.h"
#include "pcf8563_rtc.h"
#include "time_source.h"

namespace admin = eego::admin;

namespace panel {
namespace {
const char* TAG = "panel";

using eego::net::AlertAction;
using eego::net::AlertActionResult;
using eego::net::AlertActionType;
using eego::net::NetStack;
using eego::settings::PowerMode;

constexpr uint32_t kNetBitWifi = 1u << 0;
constexpr uint32_t kRenderThrottleMs = 250;
constexpr int64_t kClockCheckMs = 1000;
constexpr int64_t kDoublePressSyncMs = 1000;
constexpr uint32_t kNetIdleProbeMs = 100;
constexpr int64_t kNetIdleFirstIntervalMs = 1000;
constexpr int64_t kNetIdleMaxIntervalMs = 5000;
constexpr int64_t kFastWakeBudgetMs = 20000;
constexpr int64_t kFastWakeWifiWaitMs = 12000;
constexpr int64_t kInteractiveWifiWaitMs = 15000;
constexpr int64_t kFastSleepIdleBudgetMs = 2000;
constexpr int64_t kSleepIdleBudgetMs = 8000;
constexpr int64_t kWakePressGuardMs = 1500;
constexpr size_t kSettingsRowsPerPage = static_cast<size_t>(geom::kSettingsRowsPerPage);
constexpr int64_t kDetailCheckMs = 1000;
constexpr int64_t kOnboardingCheckMs = 2000;
constexpr int64_t kStatusRefreshMs = 30000;

void localTm(const eego::settings::Settings& settings, uint64_t unix, struct tm& out) {
  const time_t local = static_cast<time_t>(static_cast<int64_t>(unix) +
                                           static_cast<int64_t>(settings.timezone_offset_min) * 60);
  gmtime_r(&local, &out);
}

struct AdminMail {
  int type;
  int reason;
  uint32_t remainingS;
};

constexpr char kWifiDiagUrl[] = "http://192.168.4.1/wifi-diag";

constexpr int kEvPortalOpened = 0;
constexpr int kEvPortalClosed = 1;
constexpr int kEvApFallbackOpened = 2;
constexpr int kEvApFallbackClosed = 3;
constexpr int kEvProvisionSucceeded = 4;
constexpr int kEvProvisionConnecting = 5;
constexpr int kEvProvisionFailed = 6;

}  // namespace

int64_t PanelApp::nowMs() const { return esp_timer_get_time() / 1000; }

void PanelApp::begin(const Deps& deps) {
  deps_ = deps;
  touchCal_.begin(deps.canvas, deps.touch);
  settings_ = eego::settings::DeviceSettings::instance().snapshot().values;
  wakeSource_ = battery::wakeSource();
  fastWake_ = deps.fastWake;
  settingsSubscription_ = eego::settings::DeviceSettings::instance().subscribe(
      &PanelApp::onSettingsChanged, this);
  if (settingsSubscription_ < 0) ESP_LOGW(TAG, "settings subscription table full");
  adminQueue_ = xQueueCreate(8, sizeof(AdminMail));

  charging_ = eego::PowerManager::instance().isCharging();
  batCache_ = eego::PowerManager::instance().readBattery();
  lastBatMs_ = nowMs();
  lastChargeMs_ = lastBatMs_;
  lastInputMs_ = lastBatMs_;
  lastFullRenderMs_ = lastBatMs_;

  usbHost_ = eego::PowerManager::instance().usbHostConnected();
  battery::RtcState& rtc = battery::rtcState();
  manualSleep_ = fastWake_ && rtc.manual_sleep != 0;
  if (!fastWake_ && rtc.manual_sleep != 0) {
    rtc.manual_sleep = 0;
    battery::commitRtcState();
  }
  if (battery::deepSleepWake(wakeSource_) && rtc.charge_suspect != 0) {
    mainsDetector_.seedChargeSuspect(true);
  }
  mains_ = computeMains() && !manualSleep_;
  ESP_LOGI(TAG, "power source: %s (reason: %s, %umV, charging=%d, usb=%d)",
           mains_ ? "mains" : "battery", battery::powerReasonName(mainsDetector_.reason()),
           batCache_.millivolts, (charging_ || batCache_.charging) ? 1 : 0, usbHost_ ? 1 : 0);
  phaseStartMs_ = lastBatMs_;
  wakeStartUnix_ = TimeSource::valid() ? TimeSource::now() : 0;
  if (battery::interactiveWake(wakeSource_)) wakePressGuardUntilMs_ = lastBatMs_ + kWakePressGuardMs;
  if (!mains_) battPhase_ = fastWake_ ? BattPhase::Fetching : BattPhase::Interactive;
  applyWifiPowerSave();

  applySettings(eego::settings::kChangedAll);

  DashboardRuntime::Deps dashboardDeps;
  dashboardDeps.canvas = deps_.canvas;
  dashboardDeps.net = &NetStack::instance();
  dashboardDeps.alerts = deps_.publisher;
  dashboardDeps.confirm_action = [this](const DashboardConfirmRequest& request) {
    armDashboardConfirm(request);
  };
  dashboard_.setGlobalRefresh(settings_.data_refresh_s * 1000);
  dashboard_.begin(dashboardDeps);
  if (page_ != Page::Provision) page_ = home();
  dashboardWasAvailable_ = dashboard_.available();
  dashboard_.setMains(dashboardLive());
  syncDoublePress(true);

  configureRefreshPolicy();
  refreshPolicy_.noteInteraction(nowMs());
  ESP_LOGI(TAG, "panel begin: mains=%d wake=%s fast=%d lang=%s bat=%umV/%u%%", mains_ ? 1 : 0,
           battery::wakeSourceName(wakeSource_), fastWake_ ? 1 : 0, settings_.language.c_str(),
           batCache_.millivolts, batCache_.percent);
  if (battPhase_ == BattPhase::Interactive) enterInteractive(nowMs());
  if (fastWake_) {
    dirty_ = false;
    forceFull_ = false;
    return;
  }
  markDirty(true);
}

bool PanelApp::updateSettings(const eego::settings::Settings& candidate) {
  uint32_t changed = 0;
  std::string error;
  if (!eego::settings::DeviceSettings::instance().update(candidate, &changed, error)) {
    showBannerFmt(4000, "%s: %s", tr(Str::ActionFailed), error.c_str());
    return false;
  }
  return true;
}

void PanelApp::applySettings(uint32_t changed) {
  if ((changed & eego::settings::kChangedLanguage) != 0) {
    setLang(settings_.language == "en" ? Lang::En : Lang::Zh);
  }
  if ((changed & eego::settings::kChangedFrontlight) != 0) {
    eego::FrontlightManager& frontlight = eego::FrontlightManager::instance();
    if (frontlight.frontlightAvailable()) {
      frontlight.setWarm(settings_.frontlight_warmth);
      frontlight.setBrightness(settings_.frontlight_enabled
                                   ? settings_.frontlight_brightness
                                   : 0);
    }
  }
  if ((changed & eego::settings::kChangedFrontlightHardware) != 0) {
    const bool visibleNow = eego::FrontlightManager::instance().frontlightAvailable();
    if (page_ == Page::Settings) {
      if (visibleNow != frontlightRowsVisible_) {
        const std::vector<SettingId> before = visibleSettingsFor(frontlightRowsVisible_);
        const std::vector<SettingId> after = visibleSettingsFor(visibleNow);
        if (!after.empty()) {
          size_t next = after.size() - 1;
          if (focus_ < before.size()) {
            const SettingId focused = before[focus_];
            for (size_t i = 0; i < after.size(); ++i) {
              if (static_cast<uint8_t>(after[i]) >= static_cast<uint8_t>(focused)) {
                next = i;
                break;
              }
            }
          }
          focus_ = next;
        } else {
          focus_ = 0;
        }
      }
      markDirty(true);
    }
    frontlightRowsVisible_ = visibleNow;
  }
  if ((changed & eego::settings::kChangedChargeSignal) != 0) {
    charging_ = eego::PowerManager::instance().isCharging();
    batCache_.charging = charging_;
  }
  if ((changed & eego::settings::kChangedDataRefresh) != 0) {
    dashboard_.setGlobalRefresh(settings_.data_refresh_s * 1000);
  }
  if ((changed & eego::settings::kChangedEpdRefresh) != 0) {
    lastFullRenderMs_ = nowMs();
  }
  if ((changed & eego::settings::kChangedSleep) != 0) {
    lastScheduleCheckMs_ = nowMs() - 1001;
  }
  if ((changed & eego::settings::kChangedBle) != 0) {
    eego::ble::BleManager::instance().configure(settings_);
  }
  if ((changed & eego::settings::kChangedUplink) != 0) {
    eego::net::uplink::setMode(static_cast<eego::net::uplink::Mode>(settings_.uplink));
  }
  if ((changed & (eego::settings::kChangedIdleRefresh | eego::settings::kChangedTimezone)) != 0) {
    configureRefreshPolicy();
  }
  if (!fastWake_ &&
      (changed & (eego::settings::kChangedPower | eego::settings::kChangedBle)) != 0) {
    eego::ble::BleManager::instance().onPowerMode(mains_);
  }
  markDirty((changed & (eego::settings::kChangedLanguage |
                        eego::settings::kChangedTimezone)) != 0);
}

void PanelApp::onSettingsChanged(const eego::settings::Settings& values, uint32_t changed,
                                 void* ctx) {
  PanelApp* self = static_cast<PanelApp*>(ctx);
  self->settings_ = values;
  self->applySettings(changed);
  if (changed != 0) self->noteInteraction(self->nowMs());
}

battery::WakeInputs PanelApp::wakeInputs(uint64_t now_unix) const {
  battery::WakeInputs in;
  in.now_unix = now_unix;
  in.tz_offset_min = settings_.timezone_offset_min;
  in.interval_min = settings_.battery_refresh_min;
  in.night.start_min = settings_.sleep_start_min;
  in.night.end_min = settings_.sleep_end_min;
  in.battery_known = !mains_ && batCache_.millivolts > 0;
  in.battery_pct = batCache_.percent;
  in.charging = chargingShown();
  in.consecutive_failures = battery::rtcState().consecutive_failures;
  return in;
}

uint64_t PanelApp::plannedWakeAt() {
  const uint64_t now_unix = TimeSource::valid() ? TimeSource::now() : 0;
  const battery::WakePlan plan = battery::planNextWake(wakeInputs(now_unix));
  if (mains_ && plan.kind != battery::WakeKind::NightPause) return 0;
  return plan.wake_unix;
}

void PanelApp::notifyNetEvent(int wifiState) {
  wifiState_.store(wifiState);
  netBits_.fetch_or(kNetBitWifi);
}

void PanelApp::notifyAdminEvent(int type, int reason, uint32_t remainingS) {
  if (adminQueue_ == nullptr) return;
  AdminMail mail{type, reason, remainingS};
  xQueueSend(static_cast<QueueHandle_t>(adminQueue_), &mail, 0);
}

void PanelApp::drainNetBits() {
  const uint32_t bits = netBits_.exchange(0);
  if (bits == 0) return;
  netStatusCacheMs_ = 0;
  if (page_ != Page::Dashboard) markDirty();
}

void PanelApp::serviceApFallback(int64_t now) {
  if (fastWake_ || now - apFallbackCheckMs_ < 2000) return;
  apFallbackCheckMs_ = now;
  admin::ProvisionPortal& portal = admin::ProvisionPortal::instance();
  eego::net::WifiManager& wifi = eego::net::WifiManager::instance();
  const bool connected = wifi.isConnected();
  if (portal.isApFallbackActive()) {
    // On battery an open hotspot blocks deep sleep; the automatic one must not
    // drain the battery while nobody uses it.
    if (apFallbackAuto_ && !mains_ && portal.apClientCount() == 0) {
      ESP_LOGI(TAG, "on battery: closing automatic AP fallback");
      portal.closeApFallback("battery");
      apFallbackAuto_ = false;
      apConnectedSinceMs_ = -1;
      return;
    }
    if (!apFallbackAuto_ || !connected) {
      apConnectedSinceMs_ = -1;
      return;
    }
    if (apConnectedSinceMs_ < 0) apConnectedSinceMs_ = now;
    const int64_t grace = portal.apClientCount() > 0 ? 120000 : 10000;
    if (now - apConnectedSinceMs_ >= grace) {
      ESP_LOGI(TAG, "wifi connected: closing automatic AP fallback");
      portal.closeApFallback("wifi connected");
      apFallbackAuto_ = false;
      apConnectedSinceMs_ = -1;
    }
    return;
  }
  apFallbackAuto_ = false;
  apConnectedSinceMs_ = -1;
  if (connected) {
    fallbackArmed_ = false;
    return;
  }
  if (fallbackArmed_ || portal.apActive() || !admin::adminApiRunning()) return;
  if (!admin::ConfigStore::instance().admin().auto_ap_fallback) return;
  if (!mains_) return;  // battery: sleep instead; the hotspot can be opened from Settings
  if (!wifi.apFallbackDue(now)) return;
  fallbackArmed_ = true;
  ESP_LOGW(TAG, "wifi offline after %u failed passes: opening AP fallback",
           static_cast<unsigned>(wifi.diagnostics().failed_passes));
  if (portal.openApFallback() == ESP_OK) apFallbackAuto_ = true;
}

void PanelApp::drainAdminMailbox() {
  if (adminQueue_ == nullptr) return;
  AdminMail mail;
  while (xQueueReceive(static_cast<QueueHandle_t>(adminQueue_), &mail, 0) == pdTRUE) {
    noteInteraction(nowMs());
    switch (mail.type) {
      case kEvPortalOpened:
        enterProvision();
        break;
      case kEvPortalClosed:
        if (page_ == Page::Provision) {
          leaveProvision();
          if (mail.reason == 0) showBanner(tr(Str::ProvClosed), 10000);
        }
        break;
      case kEvProvisionConnecting:
      case kEvProvisionFailed:
        if (page_ == Page::Provision) markDirty();
        break;
      case kEvApFallbackOpened:
        netStatusCacheMs_ = 0;
        showBanner(tr(Str::RecoveryApStarted), 600000);
        break;
      case kEvApFallbackClosed:
        netStatusCacheMs_ = 0;
        showBanner(tr(Str::SetupHotspotClosed), 4000);
        markDirty();
        break;
      case kEvProvisionSucceeded:
        page_ = Page::Provision;
        provisionSucceeded_ = true;
        markDirty(true);
        break;
      default:
        break;
    }
  }
}

void PanelApp::service() {
  const int64_t now = nowMs();

  if (restartRequested_) {
    ESP_LOGI(TAG, "button during battery wake: restarting into interactive mode");
    NetStack::instance().wifi().powerOff();
    battery::commitRtcState();
    esp_restart();
  }

  drainAdminMailbox();
  drainNetBits();
  serviceApFallback(now);
  serviceKeepAwake(now);
  if (!mains_ &&
      eego::net::wifiAdminActive(now, admin::lastAuthenticatedRequestMs()) != adminActivePs_) {
    applyWifiPowerSave();
  }

  if (deps_.publisher != nullptr) {
    auto snap = deps_.publisher->acquire();
    if (snap && snap->seq != snapshotSeq_) {
      snapshot_ = snap;
      snapshotSeq_ = snap->seq;
      if (page_ == Page::AlertDetail || page_ == Page::Status) markDirty();
    }
  }

  refreshBattery(now);
  if (now - lastChargeMs_ > 5000) {
    charging_ = eego::PowerManager::instance().isCharging();
    usbHost_ = eego::PowerManager::instance().usbHostConnected();
    lastChargeMs_ = now;
  }

  if (confirm_ != ConfirmKind::None &&
      (confirmState_.expired(now * 1000) || !confirmTargetCurrent())) {
    clearConfirm();
  }
  flushNavigation(now * 1000);
  if (!banner_.empty() && now > bannerDeadlineMs_) {
    banner_.clear();
    markDirty();
  }

  const bool wantMains = computeMains();
  if (wantMains != mains_ && !manualSleep_) {
    if (fastWake_) {
      ESP_LOGI(TAG, "external power during battery wake: restarting");
      NetStack::instance().wifi().powerOff();
      battery::commitRtcState();
      esp_restart();
    }
    onModeChange(wantMains);
  }
  dashboard_.service(dashboardLive());
  if (dashboard_.consumeConfigChanged()) noteInteraction(now);
  const bool dashboardAvailable = dashboard_.available();
  if (dashboardAvailable != dashboardWasAvailable_) {
    dashboardWasAvailable_ = dashboardAvailable;
    if ((isHomePage(page_) || page_ == Page::AlertDetail) && page_ != home()) gotoPage(home());
  }
  if (page_ == Page::Dashboard) {
    if (dashboard_.consumeExitRequest()) dashboard_.home();
    std::string panel_id;
    std::string identity;
    if (dashboard_.consumeDetailRequest(panel_id, identity)) openDetail(panel_id, identity);
  }
  if (page_ == Page::AlertDetail && now - lastDetailCheckMs_ >= kDetailCheckMs) {
    lastDetailCheckMs_ = now;
    refreshDetailItems();
    if (detailSignature() != detailRendered_) markDirty();
  }
  if (page_ == Page::Onboarding && now - lastOnboardingCheckMs_ >= kOnboardingCheckMs) {
    lastOnboardingCheckMs_ = now;
    OnboardingModel probe;
    buildOnboarding(probe);
    const std::string key = probe.url + "|" + probe.network + "|" +
                            (probe.apMode ? "ap" : "sta") + "|" + probe.reason;
    if (key != onboardingKey_) markDirty();
  }
  if (page_ == Page::Status && now - lastStatusRenderMs_ >= kStatusRefreshMs) markDirty();

  if (!doublePressSynced_ || now - lastDoublePressSyncMs_ >= kDoublePressSyncMs ||
      pageContext() != doublePressContext_) {
    syncDoublePress(false);
  }
  if (!fastWake_) {
    const int64_t last_admin = admin::lastAuthenticatedRequestMs();
    if (eego::net::wifiAdminActive(now, last_admin)) {
      eego::ble::BleManager::instance().deferObserverUntil(last_admin +
                                                           eego::net::kWifiAdminActiveMs);
      NetStack::instance().wifi().deferRoamUntil(last_admin + eego::net::kWifiAdminActiveMs);
    }
    eego::ble::BleManager::instance().setAdminAllowed(
        settings_.ble_admin_enabled && !sleepPending_ &&
        (mains_ || battPhase_ == BattPhase::Interactive || page_ == Page::Provision));
    eego::ble::BleManager::instance().service(mains_);
  }
  serviceBlePairing(now);

  if (now - lastScheduleCheckMs_ > 1000) {
    lastScheduleCheckMs_ = now;
    if (battPhase_ == BattPhase::Mains && !sleepPending_ &&
        !battery::keepAwakeBlocks(now, keepAwakeUntil(now)) &&
        !admin::ProvisionPortal::instance().apActive() &&
        !eego::ble::BleManager::instance().sleepBlocked()) {
      uint64_t wake_at = 0;
      uint32_t remaining = 0;
      if (eego::settings::scheduledSleep(settings_, nowUnix(), wake_at, remaining)) {
        requestSleep(now, "night pause");
      }
    }
  }
  serviceScreens(now);
  batteryService(now);
  serviceSleep(now);
  if (battPhase_ == BattPhase::Fetching) return;
  if (serviceTouchCalibration(now)) return;

  if (settings_.epd_full_refresh_s > 0 &&
      now - lastFullRenderMs_ >= static_cast<int64_t>(settings_.epd_full_refresh_s) * 1000) {
    if (page_ == Page::Dashboard) {
      dashboard_.invalidateAll();
      forceFull_ = true;
    } else {
      markDirty(true);
    }
  }

  if (now - lastDeviceStateMs_ >= 1000) syncDeviceState(now, false);
  if (now - lastPowerPublishMs_ >= 2000) publishPowerState(now);

  if (page_ == Page::Dashboard) {
    if (touchMarkPending_ && now - lastRenderMs_ >= kRenderThrottleMs) {
      touchMarkPending_ = false;
      dashboard_.invalidateAll();
      dashboard_.renderOffscreen();
      drawTouchMark(*deps_.canvas);
      deps_.canvas->display(eego::RefreshMode::Fast);
      deps_.canvas->waitRefreshComplete();
      displayedThisBoot_ = true;
      noteInputResume();
      dirty_ = false;
      forceFull_ = false;
      lastRenderMs_ = now;
      return;
    }
    if (dashboard_.needsRender() && displayAllowed(now)) {
      syncDeviceState(now, true);
      dashboard_.render();
      rotateShown_ = true;
      displayedThisBoot_ = true;
      noteDisplayed();
      noteInputResume();
      if (forceFull_) lastFullRenderMs_ = now;
      dirty_ = false;
      forceFull_ = false;
      lastRenderMs_ = now;
    }
    return;
  }

  if (now - lastClockCheckMs_ >= kClockCheckMs) {
    lastClockCheckMs_ = now;
    const int64_t key =
        TimeSource::valid() ? static_cast<int64_t>(TimeSource::now() / 60) : -1;
    if (key != renderedClockKey_) markDirty();
  }

  if (dirty_ && now - lastRenderMs_ >= kRenderThrottleMs && displayAllowed(now)) {
    syncDeviceState(now, true);
    render(now);
    displayedThisBoot_ = true;
    noteDisplayed();
  }
}

void PanelApp::configureRefreshPolicy() {
  RefreshPolicyConfig config;
  config.idle_after_min = mains_ ? settings_.idle_after_min : 0;
  config.idle_refresh_min = settings_.idle_refresh_min;
  config.critical_immediate = settings_.idle_critical_immediate;
  config.timezone_offset_min = settings_.timezone_offset_min;
  refreshPolicy_.configure(config);
}

void PanelApp::noteInteraction(int64_t now) {
  if (idleLogged_) {
    ESP_LOGI(TAG, "display: idle -> active (interaction)");
    idleLogged_ = false;
  }
  refreshPolicy_.noteInteraction(now);
}

bool PanelApp::displayAllowed(int64_t now) {
  if (page_ == Page::Provision || page_ == Page::BlePair) {
    return true;
  }
  if (!refreshPolicy_.idle(now)) {
    idleAnnounced_ = false;
    return refreshPolicy_.allow(now, 0, false);
  }
  if (!idleLogged_) {
    idleLogged_ = true;
    ESP_LOGI(TAG, "display: active -> idle (refresh every %u min on the boundary)",
             static_cast<unsigned>(settings_.idle_refresh_min));
  }
  if (!idleAnnounced_) {
    idleAnnounced_ = true;
    return true;
  }
  bool critical = false;
  if (settings_.idle_critical_immediate && now - lastCriticalCheckMs_ >= 1000) {
    lastCriticalCheckMs_ = now;
    critical = refreshPolicy_.criticalArrived(dashboard_.activeCriticalIdentities());
  }
  const uint64_t unix_now = TimeSource::valid() ? TimeSource::now() : 0;
  return refreshPolicy_.allow(now, unix_now, critical);
}

void PanelApp::noteDisplayed() {
  const uint64_t unix_now = TimeSource::valid() ? TimeSource::now() : 0;
  lastDisplayedUnix_ = unix_now;
  if (settings_.idle_critical_immediate) {
    refreshPolicy_.noteRendered(unix_now, dashboard_.activeCriticalIdentities());
  } else {
    refreshPolicy_.noteRendered(unix_now);
  }
}

namespace {
ConfirmSource buttonSource(eego::ButtonId id) {
  switch (id) {
    case eego::ButtonId::Up: return ConfirmSource::Up;
    case eego::ButtonId::Down: return ConfirmSource::Down;
    case eego::ButtonId::Power: return ConfirmSource::Power;
  }
  return ConfirmSource::None;
}

const char* buttonTriggerControl(eego::ButtonId id) {
  switch (id) {
    case eego::ButtonId::Up: return "trigger:up";
    case eego::ButtonId::Down: return "trigger:down";
    case eego::ButtonId::Power: return "trigger:power";
  }
  return "";
}

}  // namespace

void PanelApp::noteInputResume() { inputResumeUs_ = esp_timer_get_time(); }

bool PanelApp::inputEventStale(int64_t timestampUs, int64_t nowUs) const {
  if (timestampUs <= 0 || nowUs < timestampUs) return true;
  const int64_t reference = std::max(timestampUs, inputResumeUs_);
  return nowUs > reference && nowUs - reference > eego::kInputActionMaxAgeUs;
}

void PanelApp::handleButton(const eego::ButtonEvent& ev) {
  const int64_t now_us = esp_timer_get_time();
  const size_t index = static_cast<size_t>(ev.id);
  if (index >= 3) return;
  bool stale = false;
  if (ev.action == eego::ButtonAction::Press) {
    stale = inputEventStale(ev.timestamp_us, now_us);
    buttonGestureStale_[index] = stale;
  } else if (ev.action == eego::ButtonAction::ChordPress) {
    const size_t up = static_cast<size_t>(eego::ButtonId::Up);
    const size_t down = static_cast<size_t>(eego::ButtonId::Down);
    stale = buttonGestureStale_[up] || buttonGestureStale_[down] ||
            inputEventStale(ev.timestamp_us, now_us);
  } else {
    stale = buttonGestureStale_[index] || ev.timestamp_us <= 0 || now_us < ev.timestamp_us;
  }
  if (stale) return;
  if (fastWake_) {
    restartRequested_ = true;
    return;
  }
  if (ev.action == eego::ButtonAction::Press && wakePressGuardUntilMs_ != 0 &&
      now_us / 1000 < wakePressGuardUntilMs_) {
    wakePressGuardUntilMs_ = 0;
    buttonGestureStale_[index] = true;
    noteInteraction(now_us / 1000);
    return;
  }
  lastInputMs_ = now_us / 1000;
  noteInteraction(lastInputMs_);
  if (touchCal_.active()) {
    touchCal_.handleButton(ev, lastInputMs_);
    return;
  }
  const ConfirmSource source = buttonSource(ev.id);
  if (ev.action == eego::ButtonAction::Press) suppressButtonLong_[index] = false;
  if (ev.action == eego::ButtonAction::LongPress && suppressButtonLong_[index]) {
    suppressButtonLong_[index] = false;
    return;
  }

  if (ev.action == eego::ButtonAction::Press) {
    suppressButtonShort_[index] = false;
    if (confirm_ != ConfirmKind::None) {
      const std::string& wanted = confirmState_.target().control;
      std::string control;
      if (wanted.rfind("trigger:", 0) == 0) {
        control = buttonTriggerControl(ev.id);
      } else if (ev.id == eego::ButtonId::Power) {
        control = wanted;
      } else {
        control = focusedControl();
      }
      if (confirmState_.beginPress(source, control, ev.timestamp_us, now_us)) return;
      clearConfirm();
    }
    return;
  }
  if (ev.action == eego::ButtonAction::Release) {
    if (confirm_ != ConfirmKind::None) confirmState_.noteRelease(source, ev.timestamp_us);
    return;
  }
  if (ev.action == eego::ButtonAction::LongPress) {
    if (confirm_ != ConfirmKind::None) {
      suppressButtonShort_[index] = true;
      clearConfirm();
      return;
    }
    if (ev.id == eego::ButtonId::Power) {
      armShutdown();
    } else {
      dispatchTrigger(ev.id == eego::ButtonId::Up ? eego::admin::InputTrigger::UpLong
                                                  : eego::admin::InputTrigger::DownLong,
                      now_us);
    }
    return;
  }
  if (ev.action == eego::ButtonAction::ShortPress) {
    if (suppressButtonShort_[index]) {
      suppressButtonShort_[index] = false;
      return;
    }
    if (confirm_ != ConfirmKind::None) {
      if (confirmState_.pressing() && confirmState_.completePress(source, now_us)) {
        executeArmedConfirm();
        suppressButtonShort_[index] = false;
        return;
      }
      clearConfirm();
    }
    if (ev.id == eego::ButtonId::Power) {
      const std::string focused =
          page_ == Page::Onboarding && !softFocusMoved_ ? std::string() : focusedControl();
      if (!focused.empty() && page_ != Page::Provision) {
        activeInputControl_ = focused;
        activateFocused();
      } else if (mains_ && page_ != Page::Provision) {
        activeInputControl_ = "trigger:power";
        armConfirm(ConfirmKind::Sleep);
      } else {
        sleepByPowerKey(now_us / 1000);
      }
    } else {
      dispatchTrigger(ev.id == eego::ButtonId::Up ? eego::admin::InputTrigger::UpShort
                                                  : eego::admin::InputTrigger::DownShort,
                      now_us);
    }
    return;
  }
  if (ev.action == eego::ButtonAction::DoublePress) {
    if (confirm_ != ConfirmKind::None) clearConfirm();
    if (ev.id != eego::ButtonId::Power) {
      dispatchTrigger(ev.id == eego::ButtonId::Up ? eego::admin::InputTrigger::UpDouble
                                                  : eego::admin::InputTrigger::DownDouble,
                      now_us);
    }
    return;
  }
  if (ev.action == eego::ButtonAction::ChordPress) {
    if (confirm_ != ConfirmKind::None) clearConfirm();
    dispatchTrigger(eego::admin::InputTrigger::UpDownChord, now_us);
  }
}

void PanelApp::handleTouch(const eego::TouchEvent& ev) {
  const int64_t now_us = esp_timer_get_time();
  traceTouch(ev);
  const bool front = ev.type == eego::TouchEventType::FrontPress ||
                     ev.type == eego::TouchEventType::FrontRelease ||
                     ev.type == eego::TouchEventType::FrontShort ||
                     ev.type == eego::TouchEventType::FrontLong ||
                     ev.type == eego::TouchEventType::FrontDouble;
  bool& gestureStale = front ? frontGestureStale_ : touchGestureStale_;
  bool stale = false;
  if (ev.type == eego::TouchEventType::Press || ev.type == eego::TouchEventType::FrontPress) {
    stale = inputEventStale(ev.timestamp_us, now_us);
    gestureStale = stale;
  } else {
    stale = gestureStale || ev.timestamp_us <= 0 || now_us < ev.timestamp_us;
  }
  if (stale) return;
  lastInputMs_ = now_us / 1000;
  noteInteraction(lastInputMs_);
  if (touchCal_.active()) {
    touchCal_.handleTouch(ev, lastInputMs_);
    return;
  }
  const int16_t x = static_cast<int16_t>(ev.x);
  const int16_t y = static_cast<int16_t>(ev.y);
  if (touchMarks_ &&
      (ev.type == eego::TouchEventType::Tap || ev.type == eego::TouchEventType::Hold)) {
    noteTouchMark(x, y);
  }
  if (ev.type == eego::TouchEventType::Press || ev.type == eego::TouchEventType::FrontPress) {
    suppressTouchHold_ = false;
  }
  if ((ev.type == eego::TouchEventType::Hold || ev.type == eego::TouchEventType::FrontLong) &&
      suppressTouchHold_) {
    suppressTouchHold_ = false;
    return;
  }

  switch (ev.type) {
    case eego::TouchEventType::Press:
      suppressTouchTap_ = false;
      if (confirm_ != ConfirmKind::None) {
        if (confirmState_.beginPress(ConfirmSource::Touch, touchControl(x, y),
                                     ev.timestamp_us, now_us)) {
          return;
        }
        clearConfirm();
      }
      break;
    case eego::TouchEventType::Release:
      if (confirm_ != ConfirmKind::None) {
        confirmState_.noteRelease(ConfirmSource::Touch, ev.timestamp_us);
      }
      break;
    case eego::TouchEventType::Move:
    case eego::TouchEventType::Cancel:
      if (confirm_ != ConfirmKind::None) {
        suppressTouchTap_ = true;
        clearConfirm();
      }
      break;
    case eego::TouchEventType::Hold:
      if (confirm_ != ConfirmKind::None) {
        suppressTouchTap_ = true;
        clearConfirm();
      } else {
        onTouchHold(x, y);
      }
      break;
    case eego::TouchEventType::Tap:
      if (suppressTouchTap_) {
        suppressTouchTap_ = false;
        return;
      }
      if (confirm_ != ConfirmKind::None) {
        if (confirmState_.pressing() &&
            confirmState_.completePress(ConfirmSource::Touch, now_us)) {
          executeArmedConfirm();
          suppressTouchTap_ = false;
          return;
        }
        clearConfirm();
      }
      activeInputControl_ = touchControl(x, y);
      onTouchTap(x, y);
      break;
    case eego::TouchEventType::SwipeLeft:
    case eego::TouchEventType::SwipeRight:
    case eego::TouchEventType::SwipeUp:
    case eego::TouchEventType::SwipeDown:
      if (confirm_ != ConfirmKind::None) clearConfirm();
      onTouchSwipe(ev.type, now_us);
      break;
    case eego::TouchEventType::FrontPress:
      suppressFrontShort_ = false;
      if (confirm_ != ConfirmKind::None) {
        const std::string control = confirmState_.target().control == "trigger:front"
                                        ? "trigger:front"
                                        : "";
        if (confirmState_.beginPress(ConfirmSource::Front, control, ev.timestamp_us, now_us)) {
          return;
        }
        clearConfirm();
      }
      break;
    case eego::TouchEventType::FrontRelease:
      if (confirm_ != ConfirmKind::None) {
        confirmState_.noteRelease(ConfirmSource::Front, ev.timestamp_us);
      }
      break;
    case eego::TouchEventType::FrontShort:
      if (suppressFrontShort_) {
        suppressFrontShort_ = false;
        return;
      }
      if (confirm_ != ConfirmKind::None) {
        if (confirmState_.pressing() &&
            confirmState_.completePress(ConfirmSource::Front, now_us)) {
          executeArmedConfirm();
          suppressFrontShort_ = false;
          return;
        }
        clearConfirm();
      }
      activeInputControl_ = "front.home";
      frontBack();
      break;
    case eego::TouchEventType::FrontLong:
      if (confirm_ != ConfirmKind::None) {
        suppressFrontShort_ = true;
        clearConfirm();
      } else {
        dispatchTrigger(eego::admin::InputTrigger::FrontLong, now_us);
      }
      break;
    case eego::TouchEventType::FrontDouble:
      if (confirm_ != ConfirmKind::None) clearConfirm();
      dispatchTrigger(eego::admin::InputTrigger::FrontDouble, now_us);
      break;
    default:
      break;
  }
}

void PanelApp::dispatchTrigger(eego::admin::InputTrigger trigger, int64_t now_us) {
  if (page_ == Page::Provision) return;
  switch (trigger) {
    case eego::admin::InputTrigger::UpShort:
    case eego::admin::InputTrigger::UpLong:
    case eego::admin::InputTrigger::UpDouble:
      activeInputControl_ = "trigger:up";
      break;
    case eego::admin::InputTrigger::DownShort:
    case eego::admin::InputTrigger::DownLong:
    case eego::admin::InputTrigger::DownDouble:
      activeInputControl_ = "trigger:down";
      break;
    case eego::admin::InputTrigger::UpDownChord:
      activeInputControl_ = "trigger:up_down";
      break;
    case eego::admin::InputTrigger::FrontLong:
    case eego::admin::InputTrigger::FrontDouble:
      activeInputControl_ = "trigger:front";
      break;
  }
  const auto bindings = eego::admin::InputBindings::instance().snapshot();
  eego::admin::InputBinding binding;
  if (eego::admin::resolveInputBinding(bindings.bindings, pageContext(), trigger, binding)) {
    executeBinding(binding, now_us);
  }
}

void PanelApp::executeBinding(const eego::admin::InputBinding& binding, int64_t now_us) {
  using eego::admin::InputActionKind;
  if (binding.action.kind == InputActionKind::None) return;
  if (binding.action.kind == InputActionKind::Builtin) {
    if (!eego::admin::inputBuiltinAllowed(binding.action.target)) {
      showBanner(tr(Str::ActionFailed), 3000);
      return;
    }
    executeBuiltin(binding.action.target, now_us);
    return;
  }
  if (inputBusy()) {
    showBanner(tr(Str::Busy), 2000);
    return;
  }
  eego::dashboard::ActionKind kind = eego::dashboard::ActionKind::Refresh;
  if (!dashboard_.executeActionRef(binding.action.panel_id, binding.action.action_id, &kind)) {
    showBanner(tr(Str::ActionFailed), 3000);
    return;
  }
  if (confirm_ == ConfirmKind::DashboardAction) return;
  if (dashboard_.consumeExitRequest()) {
    dashboard_.home();
    gotoPage(home());
    return;
  }
  if (kind == eego::dashboard::ActionKind::GotoPage ||
      kind == eego::dashboard::ActionKind::GotoPanel) {
    gotoPage(Page::Dashboard);
  }
}

void PanelApp::executeBuiltin(const std::string& name, int64_t now_us) {
  if (name == "focus_previous" || name == "focus_next") {
    requestNavigation(NavigationKind::Focus, name == "focus_previous" ? -1 : 1, now_us);
  } else if (name == "page_previous" || name == "page_next") {
    requestNavigation(NavigationKind::Page, name == "page_previous" ? -1 : 1, now_us);
  } else if (name == "home") {
    goHome();
  } else if (name == "settings") {
    gotoPage(Page::Settings);
  } else if (name == "help") {
    if (page_ != Page::Help) helpReturn_ = page_;
    gotoPage(Page::Help);
  } else if (name == "status") {
    gotoPage(Page::Status);
  } else if (name == "keep_awake") {
    toggleKeepAwake();
  } else if (name == "activate") {
    const std::string focused = focusedControl();
    if (!focused.empty()) {
      activeInputControl_ = focused;
      activateFocused();
    }
  } else if (name == "refresh") {
    if (inputBusy()) {
      showBanner(tr(Str::Busy), 2000);
    } else if (page_ == Page::Dashboard) {
      dashboard_.refreshCurrent();
    } else {
      doPollNow();
    }
  }
}

void PanelApp::goHome() {
  if (page_ == Page::Provision) return;
  const bool wasHome = page_ == home();
  if (page_ == Page::Dashboard || dashboard_.available()) dashboard_.home();
  if (!wasHome) {
    gotoPage(home());
  } else if (page_ == Page::Dashboard) {
    dashboard_.invalidateAll();
  }
}

void PanelApp::frontBack() {
  if (page_ == Page::Provision) return;
  if (page_ == Page::Dashboard || page_ != home()) {
    back();
  } else {
    goHome();
  }
}

bool PanelApp::inputBusy() const {
  return page_ == Page::Dashboard && !dashboard_.networkIdle();
}

uint64_t PanelApp::inputEpoch() const {
  return (pageEpoch_ << 32) ^ (dashboard_.pageEpoch() & 0xffffffffULL);
}

std::string PanelApp::pageContext() const {
  switch (page_) {
    case Page::Onboarding: return "onboarding";
    case Page::AlertDetail: return "detail";
    case Page::Status: return "status";
    case Page::Settings: return "settings";
    case Page::Help: return "help";
    case Page::Dashboard: return dashboard_.pageContext();
    case Page::Provision: return "provision";
    case Page::BlePair: return "ble_pair";
  }
  return {};
}

void PanelApp::requestNavigation(NavigationKind kind, int delta, int64_t now_us) {
  const int immediate = navigation_.submit(kind, delta, inputBusy(), inputEpoch(), now_us);
  if (immediate == 0) return;
  if (kind == NavigationKind::Focus) moveFocus(immediate);
  if (kind == NavigationKind::Page) movePage(immediate);
}

void PanelApp::flushNavigation(int64_t now_us) {
  NavigationKind kind = NavigationKind::None;
  const int delta = navigation_.flush(inputBusy(), inputEpoch(), now_us, kind);
  if (delta == 0) return;
  if (kind == NavigationKind::Focus) moveFocus(delta);
  if (kind == NavigationKind::Page) movePage(delta);
}

namespace {
size_t wrappedIndex(size_t current, int delta, size_t count) {
  if (count == 0) return 0;
  int64_t next = (static_cast<int64_t>(current) + delta) % static_cast<int64_t>(count);
  if (next < 0) next += static_cast<int64_t>(count);
  return static_cast<size_t>(next);
}

template <typename T, size_t N>
T stepChoice(const T (&list)[N], T current, int dir, bool wrap) {
  size_t idx = N;
  for (size_t i = 0; i < N; ++i) {
    if (list[i] == current) {
      idx = i;
      break;
    }
  }
  if (idx == N) {
    if (dir > 0) {
      for (size_t i = 0; i < N; ++i) {
        if (list[i] > current) return list[i];
      }
      return wrap ? list[0] : list[N - 1];
    }
    for (size_t i = N; i-- > 0;) {
      if (list[i] < current) return list[i];
    }
    return wrap ? list[N - 1] : list[0];
  }
  if (dir > 0) return idx + 1 < N ? list[idx + 1] : (wrap ? list[0] : list[idx]);
  if (dir < 0) return idx > 0 ? list[idx - 1] : (wrap ? list[N - 1] : list[idx]);
  return list[idx];
}

constexpr uint8_t kBrightnessSteps[] = {10, 25, 40, 60, 80, 100};
constexpr uint8_t kWarmthSteps[] = {0, 25, 50, 75, 100};
constexpr uint32_t kDataRefreshSteps[] = {0, 60, 300, 900, 1800, 3600};
constexpr uint32_t kEpdFullSteps[] = {0, 3600, 21600, 86400, 604800};
constexpr uint32_t kBatteryInteractiveSteps[] = {20, 30, 60, 120, 300, 600};
constexpr uint32_t kIdleAfterSteps[] = {0, 1, 2, 5, 10, 15, 30, 60};
constexpr uint32_t kIdleRefreshSteps[] = {2, 5, 10, 15, 30, 60};
constexpr int kTimezoneMinMin = -720;
constexpr int kTimezoneMaxMin = 840;

struct SleepPlanPreset {
  int start;
  int end;
};
constexpr SleepPlanPreset kSleepPlans[] = {
    {-1, -1}, {22 * 60, 6 * 60}, {23 * 60, 7 * 60}, {0, 6 * 60}, {12 * 60 + 30, 13 * 60 + 30}};
constexpr size_t kSleepPlanCount = sizeof(kSleepPlans) / sizeof(kSleepPlans[0]);

Str wakeSourceText(battery::WakeSource source) {
  switch (source) {
    case battery::WakeSource::Timer: return Str::WakeTimer;
    case battery::WakeSource::PowerKey: return Str::WakeButton;
    case battery::WakeSource::Buttons: return Str::WakeKeys;
    case battery::WakeSource::PowerOn: return Str::WakeReset;
    default: return Str::WakeOther;
  }
}

void formatEvery(uint32_t seconds, char* out, size_t cap) {
  if (seconds >= 86400 && seconds % 86400 == 0) {
    snprintf(out, cap, tr(Str::PollEveryDayFmt), static_cast<unsigned>(seconds / 86400));
  } else if (seconds >= 3600 && seconds % 3600 == 0) {
    snprintf(out, cap, tr(Str::PollEveryHourFmt), static_cast<unsigned>(seconds / 3600));
  } else if (seconds >= 60 && seconds % 60 == 0) {
    snprintf(out, cap, tr(Str::PollEveryMinFmt), static_cast<unsigned>(seconds / 60));
  } else {
    snprintf(out, cap, tr(Str::PollEveryFmt), static_cast<unsigned>(seconds));
  }
}

}  // namespace

void PanelApp::movePage(int delta) {
  switch (page_) {
    case Page::Dashboard:
      dashboard_.moveScreen(delta);
      break;
    case Page::AlertDetail:
      moveDetail(delta);
      break;
    case Page::Settings: {
      const size_t oldPage = settingsPage();
      focus_ = wrappedIndex(focus_, delta, visibleSettings().size());
      markDirty(settingsPage() != oldPage);
      break;
    }
    case Page::Status: {
      const size_t pages = statusPageCount();
      const int64_t next = std::max<int64_t>(
          0, std::min<int64_t>(static_cast<int64_t>(statusPage_) + delta,
                               static_cast<int64_t>(pages) - 1));
      if (static_cast<size_t>(next) == statusPage_) break;
      statusPage_ = static_cast<size_t>(next);
      markDirty(true);
      break;
    }
    default:
      break;
  }
}

void PanelApp::serviceBlePairing(int64_t now) {
  const eego::ble::Status ble = eego::ble::BleManager::instance().status();
  if (ble.pairing_passkey_valid) {
    if (page_ != Page::BlePair) {
      blePairReturn_ = page_;
      gotoPage(Page::BlePair);
    }
    if (blePairShownKey_ != ble.pairing_passkey + 1) {
      blePairShownKey_ = ble.pairing_passkey + 1;
      markDirty(true);
    }
    noteInteraction(now);
  } else if (page_ == Page::BlePair) {
    leaveBlePair();
    showBanner(tr(ble.link == eego::ble::LinkState::Authenticated ? Str::BleLinked
                                                                   : Str::BlePairFailed),
               4000);
  } else {
    blePairShownKey_ = 0;
  }
  if (ble.link != bleLinkSeen_) {
    const bool wasLinked = bleLinkSeen_ == eego::ble::LinkState::Authenticated;
    const bool linked = ble.link == eego::ble::LinkState::Authenticated;
    bleLinkSeen_ = ble.link;
    if (wasLinked != linked) {
      if (linked) noteInteraction(now);
      markDirty();
    }
  }
  if (ble.agent_connected != agentSeen_) {
    agentSeen_ = ble.agent_connected;
    markDirty();
  }
}

void PanelApp::leaveBlePair() {
  blePairShownKey_ = 0;
  if (blePairReturn_ == Page::Provision) {
    page_ = Page::Provision;
    ++pageEpoch_;
    markDirty(true);
    return;
  }
  const Page target = blePairReturn_ == Page::BlePair ||
                              (blePairReturn_ == Page::Dashboard && !dashboard_.available())
                          ? home()
                          : blePairReturn_;
  gotoPage(target);
}

void PanelApp::gotoPage(Page p) {
  if (page_ == p || (p == Page::Dashboard && !dashboard_.available())) return;
  page_ = p;
  ++pageEpoch_;
  navigation_.cancel();
  focus_ = 0;
  softFocusMoved_ = false;
  statusPage_ = 0;
  provisionSucceeded_ = false;
  clearConfirm();
  if (p == Page::Settings) refreshMuteRuleCount();
  if (p == Page::Dashboard) dashboard_.invalidateAll();
  if (p == Page::Onboarding) onboardingKey_.clear();
  syncDoublePress(false);
  markDirty(true);
}

void PanelApp::back() {
  switch (page_) {
    case Page::AlertDetail:
      gotoPage(home());
      break;
    case Page::Help:
      gotoPage(helpReturn_ == Page::Help || helpReturn_ == Page::Provision ||
                       (helpReturn_ == Page::Dashboard && !dashboard_.available())
                   ? home()
                   : helpReturn_);
      break;
    case Page::Status:
    case Page::Settings:
      gotoPage(home());
      break;
    case Page::BlePair:
      leaveBlePair();
      break;
    case Page::Dashboard:
      if (!dashboard_.back() && !dashboard_.atHome()) dashboard_.home();
      break;
    default:
      break;
  }
}

void PanelApp::moveFocus(int delta) {
  switch (page_) {
    case Page::Dashboard:
      dashboard_.moveFocus(delta, false);
      break;
    case Page::Settings: {
      const size_t rows = visibleSettings().size();
      const size_t pages = std::max<size_t>(1, (rows + kSettingsRowsPerPage - 1) /
                                                   kSettingsRowsPerPage);
      const int64_t current = static_cast<int64_t>(settingsPage());
      const int64_t next = std::max<int64_t>(
          0, std::min<int64_t>(current + delta, static_cast<int64_t>(pages) - 1));
      if (next == current) break;
      focus_ = std::min(static_cast<size_t>(next) * kSettingsRowsPerPage,
                        rows == 0 ? 0 : rows - 1);
      markDirty(true);
      break;
    }
    case Page::Onboarding:
    case Page::AlertDetail:
    case Page::Status:
    case Page::Help:
      focus_ = wrappedIndex(focus_, delta, softKeyCount());
      softFocusMoved_ = true;
      markDirty();
      break;
    default:
      break;
  }
}

void PanelApp::activateFocused() {
  switch (page_) {
    case Page::Onboarding:
    case Page::AlertDetail:
    case Page::Status:
    case Page::Help:
      activateSoftKey(focus_);
      break;
    case Page::Settings: {
      const std::vector<SettingId> rows = visibleSettings();
      if (focus_ < rows.size()) activateSettingRow(rows[focus_]);
      break;
    }
    case Page::Dashboard:
      dashboard_.activate();
      break;
    default:
      break;
  }
}

void PanelApp::activateSoftKey(size_t idx) {
  switch (page_) {
    case Page::Onboarding:
      if (idx == 0) gotoPage(Page::Status);
      else if (idx == 1) gotoPage(Page::Settings);
      break;
    case Page::AlertDetail: {
      const DashboardAlertItem* item = detailItem();
      const alert_core::AlertRecord* rec = detailRecord();
      const size_t n = softKeyCount();
      if (idx + 1 == n) {
        back();
      } else if (idx == 0 && item != nullptr) {
        doAck();
      } else if (idx == 1 && rec != nullptr && !rec->muted) {
        armConfirm(ConfirmKind::Mute);
      } else if (idx == 2 && rec != nullptr &&
                 (rec->state == alert_core::AlertState::Active ||
                  rec->state == alert_core::AlertState::Acked)) {
        armConfirm(ConfirmKind::Clear);
      } else if (idx == 3 && item != nullptr && !item->linked_panel_id.empty()) {
        const std::string linked = item->linked_panel_id;
        gotoPage(Page::Dashboard);
        dashboard_.gotoPanel(linked);
      }
      break;
    }
    case Page::Status:
      if (idx == 0) doPollNow();
      else if (idx == 1) toggleKeepAwake();
      else if (idx == 2) back();
      break;
    case Page::Settings:
      if (idx == 0) moveFocus(-1);
      else if (idx == 1) back();
      else if (idx == 2) moveFocus(1);
      break;
    case Page::Help:
      if (idx == 0) back();
      break;
    default:
      break;
  }
}

std::vector<SettingId> PanelApp::visibleSettings() const {
  return visibleSettingsFor(eego::FrontlightManager::instance().frontlightAvailable());
}

std::vector<SettingId> PanelApp::visibleSettingsFor(bool frontlight) {
  std::vector<SettingId> rows;
  rows.reserve(static_cast<size_t>(SettingId::Count));
  for (uint8_t i = 0; i < static_cast<uint8_t>(SettingId::Count); ++i) {
    const SettingId id = static_cast<SettingId>(i);
    if (!frontlight && (id == SettingId::Frontlight || id == SettingId::Brightness ||
                        id == SettingId::Warmth)) {
      continue;
    }
    rows.push_back(id);
  }
  return rows;
}

size_t PanelApp::settingsPage() const { return focus_ / kSettingsRowsPerPage; }

bool PanelApp::pageEdge(int delta) const {
  size_t page = 0;
  size_t pages = 1;
  if (page_ == Page::Settings) {
    const size_t rows = visibleSettings().size();
    pages = std::max<size_t>(1, (rows + kSettingsRowsPerPage - 1) / kSettingsRowsPerPage);
    page = settingsPage();
  } else if (page_ == Page::Status) {
    pages = statusPageCount();
    page = statusPage_;
  } else if (page_ == Page::AlertDetail) {
    pages = std::max<size_t>(1, detailItems_.size());
    page = std::min(detailIndex_, pages - 1);
  } else {
    return false;
  }
  return delta < 0 ? page == 0 : page + 1 >= pages;
}

bool PanelApp::settingAdjustable(SettingId id) {
  switch (id) {
    case SettingId::Language:
    case SettingId::Timezone:
    case SettingId::Frontlight:
    case SettingId::Brightness:
    case SettingId::Warmth:
    case SettingId::DataRefresh:
    case SettingId::EpdFull:
    case SettingId::IdleAfter:
    case SettingId::IdleRefresh:
    case SettingId::ScreenRotate:
    case SettingId::PowerMode:
    case SettingId::ChargeSignal:
    case SettingId::BatteryRefresh:
    case SettingId::BatteryMaxStale:
    case SettingId::BatteryInteractive:
    case SettingId::SleepPlan:
    case SettingId::Diagnostics:
    case SettingId::Bluetooth:
    case SettingId::BleObserver:
      return true;
    default:
      return false;
  }
}

bool PanelApp::adjustSettingRow(SettingId id, int dir, bool coarse, bool wrap) {
  if (!settingAdjustable(id) || dir == 0) return false;
  const int step = dir > 0 ? 1 : -1;
  eego::settings::Settings next = settings_;
  switch (id) {
    case SettingId::Language:
      next.language = settings_.language == "zh" ? "en" : "zh";
      break;
    case SettingId::Timezone: {
      int offset = settings_.timezone_offset_min + step * (coarse ? 360 : 60);
      if (offset > kTimezoneMaxMin) offset = kTimezoneMinMin;
      if (offset < kTimezoneMinMin) offset = kTimezoneMaxMin;
      next.timezone_offset_min = offset;
      break;
    }
    case SettingId::Frontlight:
    case SettingId::Brightness:
    case SettingId::Warmth:
      if (!eego::FrontlightManager::instance().frontlightAvailable()) {
        showBanner(tr(Str::FrontlightMissing), 3000);
        return true;
      }
      if (id == SettingId::Frontlight) {
        next.frontlight_enabled = !settings_.frontlight_enabled;
      } else if (id == SettingId::Brightness) {
        next.frontlight_brightness =
            stepChoice(kBrightnessSteps, settings_.frontlight_brightness, step, wrap);
      } else {
        next.frontlight_warmth = stepChoice(kWarmthSteps, settings_.frontlight_warmth, step, wrap);
      }
      break;
    case SettingId::DataRefresh:
      next.data_refresh_s = stepChoice(kDataRefreshSteps, settings_.data_refresh_s, step, wrap);
      break;
    case SettingId::EpdFull:
      next.epd_full_refresh_s =
          stepChoice(kEpdFullSteps, settings_.epd_full_refresh_s, step, wrap);
      break;
    case SettingId::IdleAfter:
      next.idle_after_min = stepChoice(kIdleAfterSteps, settings_.idle_after_min, step, wrap);
      break;
    case SettingId::IdleRefresh:
      next.idle_refresh_min =
          stepChoice(kIdleRefreshSteps, settings_.idle_refresh_min, step, wrap);
      break;
    case SettingId::ScreenRotate:
      next.screen_rotate_min = stepChoice(eego::settings::kScreenRotateChoices,
                                          settings_.screen_rotate_min, step, wrap);
      break;
    case SettingId::PowerMode: {
      constexpr PowerMode kModes[] = {PowerMode::Auto, PowerMode::Mains, PowerMode::Battery};
      next.power_mode = stepChoice(kModes, settings_.power_mode, step, true);
      break;
    }
    case SettingId::ChargeSignal: {
      using eego::settings::ChargeSignal;
      constexpr ChargeSignal kSignals[] = {ChargeSignal::Auto, ChargeSignal::ActiveHigh,
                                           ChargeSignal::ActiveLow};
      next.charge_signal = stepChoice(kSignals, settings_.charge_signal, step, true);
      break;
    }
    case SettingId::BatteryRefresh:
      next.battery_refresh_min = stepChoice(eego::settings::kBatteryRefreshChoices,
                                            settings_.battery_refresh_min, step, wrap);
      if (next.battery_max_stale_min < next.battery_refresh_min) {
        for (uint32_t choice : eego::settings::kBatteryMaxStaleChoices) {
          if (choice >= next.battery_refresh_min) {
            next.battery_max_stale_min = choice;
            break;
          }
        }
      }
      break;
    case SettingId::BatteryMaxStale: {
      uint32_t value = settings_.battery_max_stale_min;
      for (size_t guard = 0; guard < 8; ++guard) {
        value = stepChoice(eego::settings::kBatteryMaxStaleChoices, value, step, wrap);
        if (value >= settings_.battery_refresh_min) break;
      }
      if (value < settings_.battery_refresh_min) value = settings_.battery_max_stale_min;
      next.battery_max_stale_min = value;
      break;
    }
    case SettingId::BatteryInteractive:
      next.battery_interactive_timeout_s = stepChoice(
          kBatteryInteractiveSteps, settings_.battery_interactive_timeout_s, step, wrap);
      break;
    case SettingId::SleepPlan: {
      size_t idx = kSleepPlanCount;
      for (size_t i = 0; i < kSleepPlanCount; ++i) {
        if (kSleepPlans[i].start == settings_.sleep_start_min &&
            kSleepPlans[i].end == settings_.sleep_end_min) {
          idx = i;
          break;
        }
      }
      size_t target = 0;
      if (idx == kSleepPlanCount) {
        target = step > 0 ? 0 : kSleepPlanCount - 1;
      } else {
        target = static_cast<size_t>((static_cast<int64_t>(idx) + step +
                                      static_cast<int64_t>(kSleepPlanCount)) %
                                     static_cast<int64_t>(kSleepPlanCount));
      }
      next.sleep_start_min = kSleepPlans[target].start;
      next.sleep_end_min = kSleepPlans[target].end;
      break;
    }
    case SettingId::Diagnostics:
      next.diagnostics_enabled = !settings_.diagnostics_enabled;
      break;
    case SettingId::Bluetooth:
      if (!eego::ble::BleManager::compiled()) {
        showBanner(tr(Str::Unavailable), 3000);
        return true;
      }
      next.ble_admin_enabled = !settings_.ble_admin_enabled;
      break;
    case SettingId::BleObserver:
      if (!eego::ble::BleManager::instance().status().compiled) {
        showBanner(tr(Str::Unavailable), 3000);
        return true;
      }
      next.ble_observer_enabled = !settings_.ble_observer_enabled;
      break;
    default:
      return false;
  }
  if (updateSettings(next) && id == SettingId::SleepPlan && next.sleep_start_min >= 0) {
    showBanner(tr(Str::DeepSleepOffline), 5000);
  }
  markDirty();
  return true;
}

PanelApp::TouchHit PanelApp::resolveTouchHit(int16_t x, int16_t y) const {
  TouchHit hit;
  if (y >= geom::kSoftKeyY && y < geom::kSoftKeyY + geom::kSoftKeyH) {
    const size_t n = softKeyCount();
    if (n > 0) {
      const int gap = geom::kSoftKeyGap;
      const int kw = (geom::kContentW - gap * static_cast<int>(n - 1)) / static_cast<int>(n);
      int idx = (x - geom::kContentX) / (kw + gap);
      idx = std::max(0, std::min(idx, static_cast<int>(n) - 1));
      hit.kind = TouchHit::Kind::SoftKey;
      hit.index = idx;
    }
    return hit;
  }
  if (page_ == Page::Settings) {
    const int top = geom::kSettingsRowTop;
    const int pitch = geom::kSettingsRowPitch;
    if (y >= top && y < top + pitch * static_cast<int>(kSettingsRowsPerPage)) {
      const size_t row = static_cast<size_t>((y - top) / pitch);
      const size_t global = settingsPage() * kSettingsRowsPerPage + row;
      const std::vector<SettingId> rows = visibleSettings();
      if (global < rows.size()) {
        hit.kind = TouchHit::Kind::Row;
        hit.index = static_cast<int>(global);
        if (x >= geom::kSettingsValueX0 && x < geom::kContentX + geom::kContentW) {
          const int mid = (geom::kSettingsValueX0 + geom::kSettingsValueX1) / 2;
          hit.valueLeft = x < mid;
          hit.valueRight = !hit.valueLeft;
        }
      }
    }
  }
  return hit;
}

std::string PanelApp::controlForHit(const TouchHit& hit) const {
  const std::string prefix = std::to_string(static_cast<unsigned>(page_));
  switch (hit.kind) {
    case TouchHit::Kind::SoftKey:
      return prefix + ":key:" + std::to_string(hit.index);
    case TouchHit::Kind::Row:
      return prefix + ":row:" + std::to_string(hit.index + 1);
    case TouchHit::Kind::None:
      break;
  }
  return {};
}

std::string PanelApp::focusedControl() const {
  if (page_ == Page::Dashboard) return dashboard_.focusedControl();
  TouchHit hit;
  hit.index = static_cast<int>(focus_);
  if (page_ == Page::Settings) {
    hit.kind = TouchHit::Kind::Row;
    return controlForHit(hit);
  }
  if (page_ == Page::Onboarding || page_ == Page::AlertDetail || page_ == Page::Status ||
      page_ == Page::Help) {
    hit.kind = TouchHit::Kind::SoftKey;
    return controlForHit(hit);
  }
  return {};
}

std::string PanelApp::touchControl(int16_t x, int16_t y) const {
  if (page_ == Page::Dashboard) return dashboard_.touchControl(x, y);
  return controlForHit(resolveTouchHit(x, y));
}

void PanelApp::onTouchTap(int16_t x, int16_t y) {
  if (page_ == Page::Dashboard) {
    dashboard_.touch(x, y);
    return;
  }
  const TouchHit hit = resolveTouchHit(x, y);
  switch (hit.kind) {
    case TouchHit::Kind::SoftKey:
      activateSoftKey(static_cast<size_t>(hit.index));
      return;
    case TouchHit::Kind::Row: {
      const std::vector<SettingId> rows = visibleSettings();
      const size_t row = static_cast<size_t>(hit.index);
      if (row >= rows.size()) return;
      if (focus_ != row) {
        focus_ = row;
        markDirty();
      }
      const SettingId id = rows[row];
      if ((hit.valueLeft || hit.valueRight) && settingAdjustable(id)) {
        adjustSettingRow(id, hit.valueLeft ? -1 : 1);
      } else {
        activateSettingRow(id);
      }
      return;
    }
    case TouchHit::Kind::None:
      break;
  }
}

void PanelApp::onTouchHold(int16_t x, int16_t y) {
  if (page_ != Page::Settings) return;
  const TouchHit hit = resolveTouchHit(x, y);
  if (hit.kind != TouchHit::Kind::Row || !(hit.valueLeft || hit.valueRight)) return;
  const std::vector<SettingId> rows = visibleSettings();
  const size_t row = static_cast<size_t>(hit.index);
  if (row >= rows.size() || !settingAdjustable(rows[row])) return;
  if (focus_ != row) {
    focus_ = row;
    markDirty();
  }
  activeInputControl_ = controlForHit(hit);
  adjustSettingRow(rows[row], hit.valueLeft ? -1 : 1, rows[row] == SettingId::Timezone);
}

void PanelApp::onTouchSwipe(eego::TouchEventType type, int64_t now_us) {
  const bool forward =
      type == eego::TouchEventType::SwipeLeft || type == eego::TouchEventType::SwipeUp;
  const bool swipeRight = type == eego::TouchEventType::SwipeRight;
  const int delta = forward ? 1 : -1;
  switch (page_) {
    case Page::Dashboard:
      requestNavigation(NavigationKind::Page, delta, now_us);
      break;
    case Page::Settings:
      if (pageEdge(delta)) {
        if (swipeRight) back();
        break;
      }
      requestNavigation(NavigationKind::Focus, delta, now_us);
      break;
    case Page::AlertDetail:
    case Page::Status:
      if (pageEdge(delta)) {
        if (swipeRight) back();
        break;
      }
      requestNavigation(NavigationKind::Page, delta, now_us);
      break;
    case Page::Help:
      if (swipeRight) back();
      break;
    default:
      break;
  }
}

void PanelApp::syncDoublePress(bool force) {
  const std::string context = pageContext();
  const eego::admin::InputBindingSet set = eego::admin::InputBindings::instance().snapshot();
  lastDoublePressSyncMs_ = nowMs();
  if (!force && doublePressSynced_ && set.revision == doublePressRevision_ &&
      context == doublePressContext_) {
    return;
  }
  doublePressSynced_ = true;
  doublePressRevision_ = set.revision;
  doublePressContext_ = context;
  auto bound = [&](eego::admin::InputTrigger trigger) {
    if (page_ == Page::Provision) return false;
    eego::admin::InputBinding binding;
    return eego::admin::resolveInputBinding(set.bindings, context, trigger, binding) &&
           binding.action.kind != eego::admin::InputActionKind::None;
  };
  eego::ButtonManager& buttons = eego::ButtonManager::instance();
  buttons.setDoublePressEnabled(eego::ButtonId::Up, bound(eego::admin::InputTrigger::UpDouble));
  buttons.setDoublePressEnabled(eego::ButtonId::Down,
                                bound(eego::admin::InputTrigger::DownDouble));
}

void PanelApp::doAck() {
  const DashboardAlertItem* item = detailItem();
  if (item == nullptr) return;
  if (item->acknowledged) {
    showBanner(tr(Str::DoneAck), 2000);
    return;
  }
  if (!item->active) {
    showBanner(tr(Str::StateCleared), 2000);
    return;
  }
  const std::string identity = item->identity;
  if (dashboard_.acknowledgeEvent(identity)) {
    showBanner(tr(Str::DoneAck), 3000);
  } else if (!item->core_id.empty()) {
    const AlertActionResult r =
        NetStack::instance().alertAction({AlertActionType::Ack, item->core_id, "panel"});
    if (r.code == AlertActionResult::Code::Ok) {
      showBanner(tr(Str::DoneAck), 3000);
    } else {
      showBannerFmt(4000, "%s: %s", tr(Str::ActionFailed), r.detail.c_str());
    }
  } else {
    showBanner(tr(Str::ActionFailed), 3000);
  }
  refreshDetailItems();
  markDirty();
}

void PanelApp::doClear(const std::string& id) {
  AlertActionResult r = NetStack::instance().alertAction({AlertActionType::Clear, id, ""});
  if (r.code == AlertActionResult::Code::Ok) {
    showBanner(tr(Str::DoneClear), 3000);
  } else {
    showBannerFmt(4000, "%s: %s", tr(Str::ActionFailed), r.detail.c_str());
  }
  markDirty();
}

void PanelApp::doMute(const alert_core::AlertRecord& rec) {
  NetStack& net = NetStack::instance();
  std::vector<alert_core::MuteRule> rules;
  if (!net.queryMuteRules(rules)) {
    showBanner(tr(Str::ActionFailed), 3000);
    return;
  }
  bool dup = false;
  for (const auto& r : rules) {
    if (r.match_by == alert_core::MuteRule::MatchBy::Fingerprint && r.value == rec.fingerprint) {
      dup = true;
      break;
    }
  }
  if (!dup) {
    alert_core::MuteRule rule;
    rule.match_by = alert_core::MuteRule::MatchBy::Fingerprint;
    rule.value = rec.fingerprint;
    rule.expires_at = 0;
    rules.push_back(std::move(rule));
  }
  std::string err;
  if (!net.setMuteRules(rules, err)) {
    showBannerFmt(4000, "%s: %s", tr(Str::ActionFailed), err.c_str());
    return;
  }
  persistMuteRules(rules);
  muteRuleCount_ = rules.size();
  showBanner(tr(Str::DoneMute), 3000);
  refreshDetailItems();
  markDirty();
}

void PanelApp::doClearMutes() {
  std::string err;
  if (!NetStack::instance().clearMuteRules(err)) {
    showBannerFmt(4000, "%s: %s", tr(Str::ActionFailed), err.c_str());
    return;
  }
  persistMuteRules({});
  muteRuleCount_ = 0;
  showBanner(tr(Str::DoneClear), 3000);
  markDirty();
}

void PanelApp::persistMuteRules(const std::vector<alert_core::MuteRule>& rules) {
  admin::ConfigStore& cs = admin::ConfigStore::instance();
  alert_core::JsonValue dom = cs.rawDom();
  if (!cs.setMuteRulesInDom(dom, rules)) return;
  std::string err;
  if (!cs.commit(dom, err)) {
    ESP_LOGW(TAG, "mute rules persist failed (active until reboot): %s", err.c_str());
  }
}

void PanelApp::refreshMuteRuleCount() {
  std::vector<alert_core::MuteRule> rules;
  if (NetStack::instance().queryMuteRules(rules)) {
    muteRuleCount_ = rules.size();
  }
}

void PanelApp::doPollNow() {
  if (!dashboard_.available()) {
    showBanner(tr(Str::NoDashboard), 3000);
    return;
  }
  dashboard_.refreshAll();
  showBanner(tr(Str::PollNowStarted), 3000);
  markDirty();
}

void PanelApp::activateSettingRow(SettingId id) {
  if (settingAdjustable(id)) {
    adjustSettingRow(id, 1, false, true);
    return;
  }
  switch (id) {
    case SettingId::KeepAwake:
      toggleKeepAwake();
      break;
    case SettingId::Network: {
      const eego::net::NetStatusSnapshot& status = netStatus();
      if (!status.wifi.ip.empty()) showBannerFmt(5000, "http://%s/", status.wifi.ip.c_str());
      else showBanner(tr(Str::WifiDisconnected), 3000);
      break;
    }
    case SettingId::RecoveryAp: armConfirm(ConfirmKind::NetworkRecovery); break;
    case SettingId::WifiReset: armConfirm(ConfirmKind::WifiReset); break;
    case SettingId::PollNow: doPollNow(); break;
    case SettingId::EpdNow:
      markDirty(true);
      showBanner(tr(Str::FullRefreshQueued), 3000);
      break;
    case SettingId::NextWake: {
      const uint64_t wake = plannedWakeAt();
      if (wake == 0) showBanner(tr(Str::NotScheduled), 3000);
      else {
        char text[32];
        formatTs(wake, text, sizeof(text));
        showBanner(text, 4000);
      }
      break;
    }
    case SettingId::LastWake: {
      showBanner(tr(wakeSourceText(wakeSource_)), 3000);
      break;
    }
    case SettingId::Storage:
      showBanner(eego::SdCard::instance().ready() ? tr(Str::SdReady) : tr(Str::SdMissing), 3000);
      break;
    case SettingId::RunDiagnostics:
      if (settings_.diagnostics_enabled) diagSmoke();
      else showBanner(tr(Str::Unavailable), 3000);
      break;
    case SettingId::Security:
      showBanner(admin::AuthManager::instance().hasPassword() ? tr(Str::SecuritySet)
                                                              : tr(Str::SecurityMissing),
                 3000);
      break;
    case SettingId::Mutes: armConfirm(ConfirmKind::ClearMutes); break;
    case SettingId::FactoryReset: armConfirm(ConfirmKind::FactoryReset); break;
    case SettingId::Version: {
      const esp_app_desc_t* description = esp_app_get_description();
      showBannerFmt(5000, "%s %s", description->project_name, description->version);
      break;
    }
    case SettingId::InputHelp:
      helpReturn_ = Page::Settings;
      gotoPage(Page::Help);
      break;
    case SettingId::InputReset: resetInputBindings(); break;
    case SettingId::TouchCal: startTouchCalibration(); break;
    case SettingId::TouchMarks:
      touchMarks_ = !touchMarks_;
      touchMarkX_ = touchMarkY_ = -1;
      touchMarkPending_ = false;
      markDirty();
      break;
    case SettingId::BleUnbind:
      if (!eego::ble::BleManager::instance().status().compiled) {
        showBanner(tr(Str::Unavailable), 3000);
      } else {
        armConfirm(ConfirmKind::BleUnbind);
      }
      break;
    default: break;
  }
}

void PanelApp::doOpenRecovery() {
  admin::ProvisionPortal& portal = admin::ProvisionPortal::instance();
  esp_err_t err = ESP_OK;
  if (!portal.apActive()) {
    err = admin::AuthManager::instance().hasPassword() ? portal.openApFallback() : portal.open();
  }
  if (err != ESP_OK && !(err == ESP_ERR_INVALID_STATE && portal.apActive())) {
    ESP_LOGW(TAG, "setup hotspot failed: %s", esp_err_to_name(err));
    showBanner(tr(Str::SetupHotspotFailed), 4000);
    return;
  }
  noteInteraction(nowMs());
  if (portal.isApFallbackActive()) showBanner(tr(Str::RecoveryApStarted), 600000);
  markDirty(true);
}

void PanelApp::doClearBleBonds() {
  const esp_err_t err = eego::ble::BleManager::instance().clearBonds();
  if (err == ESP_OK) {
    showBanner(tr(Str::BleUnbindDone), 4000);
  } else {
    ESP_LOGW(TAG, "BLE clear bonds failed: %s", esp_err_to_name(err));
    showBanner(tr(Str::ActionFailed), 4000);
  }
  markDirty(true);
}

void PanelApp::doWifiReset() {
  admin::AdminCommand cmd;
  cmd.type = admin::AdminCommandType::WifiReset;
  admin::AdminResult result;
  admin::defaultCommandHandler(cmd, result, nullptr);
  if (result.ok) {
    showBanner(tr(Str::WifiResetDone), 600000);
  } else {
    ESP_LOGW(TAG, "wifi reset failed: %s", result.err.c_str());
    showBanner(tr(Str::ActionFailed), 5000);
  }
  markDirty(true);
}

void PanelApp::doFactoryReset() {
  admin::AdminCommand cmd;
  cmd.type = admin::AdminCommandType::FactoryReset;
  cmd.delay_ms = 3000;
  admin::AdminResult result;
  admin::defaultCommandHandler(cmd, result, nullptr);
  if (result.ok) {
    eego::FrontlightManager::instance().off();
    showBanner(tr(Str::FactoryResetDone), 3000);
  } else {
    ESP_LOGW(TAG, "factory reset failed: %s", result.err.c_str());
    showBanner(tr(Str::ActionFailed), 5000);
  }
  markDirty(true);
}

uint64_t PanelApp::currentSnapshotEpoch() const {
  if (deps_.publisher != nullptr) {
    const auto snapshot = deps_.publisher->acquire();
    if (snapshot != nullptr) return snapshot->seq;
  }
  return snapshotSeq_;
}

bool PanelApp::renderDashboardPreview(const admin::RenderPreviewRequest& request, uint8_t* rows,
                                      size_t stride, admin::RenderPreviewResult& result) {
  if (request.document == nullptr || rows == nullptr) {
    result.status = 503;
    result.error = "render preview unavailable";
    return false;
  }
  // Separate PSRAM framebuffer (no SPI/panel); freed when this scope ends.
  eego::Uc8279cDisplay offscreen;
  if (offscreen.beginOffscreen() != ESP_OK) {
    result.status = 503;
    result.error = "preview framebuffer unavailable";
    return false;
  }
  eego::PortraitCanvas canvas(offscreen);
  DashboardRuntime::PreviewInfo info;
  const bool ok =
      dashboard_.renderPreview(canvas, request.document->dashboard, request.page_id, info);
  result.page_id = info.page_id;
  result.screen = static_cast<uint32_t>(info.screen);
  result.screen_count = static_cast<uint32_t>(info.screen_count);
  result.data = info.data;
  if (!ok) {
    result.status = info.bad_request ? 400 : 503;
    result.error = info.error.empty() ? "render preview failed" : info.error;
    return false;
  }
  eego::dashboard::ui::packGrayLevels(offscreen, rows, stride, request.fast);
  noteInputResume();  // the render blocked app_task; do not drop queued input as stale
  return true;
}

void PanelApp::resetInputBindings() {
  std::string error;
  bool conflict = false;
  const auto document = admin::DashboardStore::instance().snapshot();
  const eego::dashboard::Dashboard* current =
      document != nullptr && document->configured ? &document->dashboard : nullptr;
  if (admin::InputBindings::instance().reset(false, 0, current, error, conflict)) {
    showBanner(tr(Str::InputResetDone), 3000);
    syncDoublePress(true);
  } else {
    showBannerFmt(4000, "%s: %s", tr(Str::ActionFailed), error.c_str());
  }
  markDirty();
}

bool PanelApp::confirmTargetCurrent() const {
  if (confirm_ == ConfirmKind::None || !confirmState_.armed()) return false;
  const ConfirmTarget& target = confirmState_.target();
  if (confirm_ == ConfirmKind::DashboardAction) {
    return page_ == Page::Dashboard && target.page_epoch == pageEpoch_ &&
           dashboard_.pageEpoch() == confirmDashboard_.page_epoch &&
           dashboard_.snapshotEpoch() == confirmDashboard_.snapshot_epoch;
  }
  if (target.page_epoch != pageEpoch_) return false;
  if (confirm_ == ConfirmKind::Mute || confirm_ == ConfirmKind::Clear) {
    const auto latest = deps_.publisher != nullptr ? deps_.publisher->acquire() : snapshot_;
    const ConfirmRecordRule rule =
        confirm_ == ConfirmKind::Mute ? ConfirmRecordRule::Mute : ConfirmRecordRule::Clear;
    if (!confirmRecordTargetCurrent(target, rule, latest.get())) return false;
    const DashboardAlertItem* item = detailItem();
    if (page_ != Page::AlertDetail || item == nullptr || item->core_id != target.record_id) {
      return false;
    }
  }
  return true;
}

void PanelApp::executeArmedConfirm() {
  if (!confirmTargetCurrent()) {
    clearConfirm();
    showBanner(tr(Str::ActionFailed), 3000);
    return;
  }
  if (confirm_ == ConfirmKind::DashboardAction) {
    const DashboardConfirmRequest request = confirmDashboard_;
    clearConfirm();
    dashboard_.executeConfirmed(request);
    return;
  }
  switch (confirm_) {
    case ConfirmKind::Mute: {
      const auto latest = deps_.publisher != nullptr ? deps_.publisher->acquire() : snapshot_;
      const alert_core::AlertRecord* rec = nullptr;
      if (latest != nullptr && latest->seq == confirmState_.target().snapshot_epoch) {
        for (const auto& candidate : latest->records) {
          if (candidate.id == confirmState_.target().record_id) {
            rec = &candidate;
            break;
          }
        }
      }
      if (rec != nullptr) doMute(*rec);
      else showBanner(tr(Str::ActionFailed), 3000);
      break;
    }
    case ConfirmKind::Clear: doClear(confirmState_.target().record_id); break;
    case ConfirmKind::ClearMutes: doClearMutes(); break;
    case ConfirmKind::NetworkRecovery: doOpenRecovery(); break;
    case ConfirmKind::WifiReset: doWifiReset(); break;
    case ConfirmKind::FactoryReset: doFactoryReset(); break;
    case ConfirmKind::BleUnbind: doClearBleBonds(); break;
    case ConfirmKind::Shutdown: shutdownNow(); break;
    case ConfirmKind::Sleep:
      clearConfirm();
      sleepByPowerKey(nowMs());
      return;
    default: break;
  }
  clearConfirm();
}

void PanelApp::armConfirm(ConfirmKind kind) {
  clearConfirm();
  if (activeInputControl_.empty()) {
    showBanner(tr(Str::ActionFailed), 3000);
    return;
  }
  ConfirmTarget target;
  target.page_epoch = pageEpoch_;
  target.control = activeInputControl_;
  target.physical_power_only = requiresPhysicalConfirm(kind);
  if (kind == ConfirmKind::Mute || kind == ConfirmKind::Clear) {
    const DashboardAlertItem* item = detailItem();
    if (item == nullptr || item->core_id.empty()) {
      showBanner(tr(Str::Unavailable), 3000);
      return;
    }
    target.snapshot_epoch = currentSnapshotEpoch();
    target.record_id = item->core_id;
  }
  confirm_ = kind;
  confirmState_.arm(target, esp_timer_get_time());
  showBanner(kind == ConfirmKind::Shutdown       ? tr(Str::ConfirmShutdown)
             : kind == ConfirmKind::Sleep      ? tr(Str::ConfirmSleep)
             : requiresPhysicalConfirm(kind) ? tr(Str::ConfirmPhysicalPress)
                                             : tr(Str::ConfirmPress),
             static_cast<uint32_t>(kConfirmWindowUs / 1000));
}

void PanelApp::armDashboardConfirm(const DashboardConfirmRequest& request) {
  clearConfirm();
  if (activeInputControl_.empty()) {
    showBanner(tr(Str::ActionFailed), 3000);
    return;
  }
  DashboardConfirmRequest prepared = request;
  if (activeInputControl_ == "trigger:up_down") {
    if (!dashboard_.focusAction(request.panel_id, request.action_id, request.target_identity)) {
      showBanner(tr(Str::ActionFailed), 3000);
      return;
    }
    activeInputControl_ = dashboard_.focusedControl();
    prepared.page_epoch = dashboard_.pageEpoch();
  }
  if (page_ != Page::Dashboard) {
    const bool trigger = activeInputControl_.rfind("trigger:", 0) == 0;
    const bool focused = activeInputControl_ == dashboard_.focusedControl();
    if (!trigger && !focused) {
      showBanner(tr(Str::ActionFailed), 3000);
      return;
    }
    gotoPage(Page::Dashboard);
  }
  ConfirmTarget target;
  target.page_epoch = pageEpoch_;
  target.snapshot_epoch = prepared.snapshot_epoch;
  target.control = activeInputControl_;
  target.record_id = prepared.target_identity;
  confirmDashboard_ = std::move(prepared);
  confirm_ = ConfirmKind::DashboardAction;
  confirmState_.arm(target, esp_timer_get_time());
  showBanner(tr(Str::ConfirmPress), static_cast<uint32_t>(kConfirmWindowUs / 1000));
}

void PanelApp::clearConfirm() {
  if (confirm_ == ConfirmKind::None) return;
  if (confirmState_.pressing()) {
    switch (confirmState_.source()) {
      case ConfirmSource::Up:
        suppressButtonShort_[0] = suppressButtonLong_[0] = true;
        break;
      case ConfirmSource::Down:
        suppressButtonShort_[1] = suppressButtonLong_[1] = true;
        break;
      case ConfirmSource::Power:
        suppressButtonShort_[2] = suppressButtonLong_[2] = true;
        break;
      case ConfirmSource::Touch:
        suppressTouchTap_ = suppressTouchHold_ = true;
        break;
      case ConfirmSource::Front:
        suppressFrontShort_ = suppressTouchHold_ = true;
        break;
      default: break;
    }
  }
  confirm_ = ConfirmKind::None;
  confirmState_.cancel();
  confirmDashboard_ = DashboardConfirmRequest{};
  if (banner_ == tr(Str::ConfirmPress) || banner_ == tr(Str::ConfirmPhysicalPress) ||
      banner_ == tr(Str::ConfirmShutdown)) {
    banner_.clear();
  }
  if (page_ == Page::Dashboard) dashboard_.showStatusMessage("", 1);
  markDirty();
}

void PanelApp::showBanner(const char* text, uint32_t ms) {
  if (dashboardUsesRuntimeStatus(page_)) {
    dashboard_.showStatusMessage(text != nullptr ? text : "", ms);
    return;
  }
  banner_ = text != nullptr ? text : "";
  sanitizeClamped(banner_, 90);
  bannerDeadlineMs_ = nowMs() + ms;
  markDirty();
}

void PanelApp::showBannerFmt(uint32_t ms, const char* fmt, ...) {
  char buf[96];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  showBanner(buf, ms);
}

bool PanelApp::computeMains() {
  battery::PowerSample sample;
  sample.charging = charging_ || batCache_.charging;
  sample.usb_host = usbHost_;
  sample.battery_mv = batCache_.millivolts;
  sample.battery_read_ms = lastBatMs_;
  return mainsDetector_.update(static_cast<uint8_t>(settings_.power_mode), sample, nowMs());
}

void PanelApp::onModeChange(bool mains) {
  mains_ = mains;
  configureRefreshPolicy();
  noteInteraction(nowMs());
  ESP_LOGI(TAG, "power mode -> %s (reason: %s, %umV, charging=%d, usb=%d)",
           mains ? "mains" : "battery", battery::powerReasonName(mainsDetector_.reason()),
           batCache_.millivolts, (charging_ || batCache_.charging) ? 1 : 0, usbHost_ ? 1 : 0);
  if (mainsDetector_.reason() == battery::PowerReason::ChargeSuspect) {
    ESP_LOGW(TAG, "charge pin reports charging but the battery is draining: running on "
                  "battery (check Settings > Charge signal)");
  }
  eego::ble::BleManager::instance().onPowerMode(mains);
  eego::ble::BleManager::instance().setBatteryScanWindow(false);
  if (mains) {
    battPhase_ = BattPhase::Mains;
    sleepPending_ = false;
    sleepIdleWait_.active = false;
    NetStack::instance().reconnectNow();
  } else {
    enterInteractive(nowMs());
  }
  dashboard_.setMains(dashboardLive());
  applyWifiPowerSave();
  markDirty(true);
}

void PanelApp::applyWifiPowerSave() {
  const int64_t now = nowMs();
  adminActivePs_ = eego::net::wifiAdminActive(now, admin::lastAuthenticatedRequestMs());
  NetStack::instance().wifi().setPowerSave(eego::net::wifiModemSleepWanted(
      mains_, keepAwakeUntil(now) != 0, battPhase_ == BattPhase::Interactive, adminActivePs_));
}

void PanelApp::enterInteractive(int64_t now) {
  battPhase_ = BattPhase::Interactive;
  phaseStartMs_ = now;
  batchStarted_ = false;
  noteInteraction(now);
  dashboard_.setSleepFrame(false);
  eego::ble::BleManager::instance().setBatteryScanWindow(true);
  applyWifiPowerSave();
}

void PanelApp::startNetIdleWait(NetIdleWait& w, int64_t now, int64_t budgetMs) {
  w.active = true;
  w.deadlineMs = now + budgetMs;
  w.nextProbeMs = now;
  w.intervalMs = kNetIdleFirstIntervalMs;
}

int PanelApp::pollNetIdle(NetIdleWait& w, int64_t now) {
  if (!w.active) return 1;
  if (now < w.nextProbeMs) return 0;
  if (NetStack::instance().waitIdle(kNetIdleProbeMs)) {
    w.active = false;
    return 1;
  }
  const int64_t after = esp_timer_get_time() / 1000;
  if (after >= w.deadlineMs) {
    w.active = false;
    return -1;
  }
  w.nextProbeMs = after + w.intervalMs;
  w.intervalMs = std::min<int64_t>(w.intervalMs * 2, kNetIdleMaxIntervalMs);
  return 0;
}

void PanelApp::batteryService(int64_t now) {
  if (sleepPending_) return;
  const bool wifi_up = wifiState_.load() == 2 /*Connected*/;
  if (wifi_up) wifiSeen_ = true;
  switch (battPhase_) {
    case BattPhase::Mains:
      return;
    case BattPhase::Fetching: {
      if (!dashboard_.available()) {
        requestSleep(now, "no dashboard");
        return;
      }
      if (!batchStarted_) {
        const bool wifi_gave_up = wifiState_.load() == 4 /*OfflineMode*/;
        if (wifi_up || wifi_gave_up || now >= kFastWakeWifiWaitMs) {
          dashboard_.pollBattery(std::max<int64_t>(1000, kFastWakeBudgetMs - now));
          batchStarted_ = true;
          ESP_LOGI(TAG, "battery wake: fetch batch started (wifi=%d, t=%lldms)", wifi_up ? 1 : 0,
                   static_cast<long long>(now));
        }
        if (now < kFastWakeBudgetMs) return;
      }
      if (dashboard_.networkIdle()) {
        requestSleep(now, "batch complete");
      } else if (now >= kFastWakeBudgetMs) {
        requestSleep(now, "wake budget exhausted");
      }
      return;
    }
    case BattPhase::Interactive: {
      if (admin::ProvisionPortal::instance().apActive() ||
          eego::ble::BleManager::instance().sleepBlocked()) {
        noteInteraction(now);
        return;
      }
      if (!batchStarted_ && (wifi_up || now - phaseStartMs_ >= kInteractiveWifiWaitMs)) {
        batchStarted_ = true;
      }
      const int64_t last = std::max(refreshPolicy_.lastInteractionMs(),
                                    admin::lastAuthenticatedRequestMs());
      if (battery::interactiveSleepDue(now, last, settings_.battery_interactive_timeout_s,
                                       keepAwakeUntil(now))) {
        requestSleep(now, "interactive timeout");
      }
      return;
    }
  }
}

void PanelApp::requestSleep(int64_t now, const char* why) {
  if (sleepPending_) return;
  ESP_LOGI(TAG, "sleep requested: %s", why);
  sleepPending_ = true;
  sleepRequestedMs_ = now;
  dashboard_.cancelBatteryCycle();
  startNetIdleWait(sleepIdleWait_, now, fastWake_ ? kFastSleepIdleBudgetMs : kSleepIdleBudgetMs);
}

void PanelApp::serviceSleep(int64_t now) {
  if (!sleepPending_) return;
  const int64_t last_input = refreshPolicy_.lastInteractionMs();
  const bool manual = manualSleep_ && !fastWake_;
  const bool cancelled =
      manual ? last_input > sleepRequestedMs_
             : battPhase_ == BattPhase::Interactive &&
                   std::max(last_input, admin::lastAuthenticatedRequestMs()) > sleepRequestedMs_;
  if (cancelled) {
    ESP_LOGI(TAG, "sleep cancelled by input");
    if (!fastWake_) manualSleep_ = false;
    sleepPending_ = false;
    sleepIdleWait_.active = false;
    dashboard_.setMains(dashboardLive());
    return;
  }
  if (battPhase_ != BattPhase::Fetching && battery::keepAwakeBlocks(now, keepAwakeUntil(now))) {
    ESP_LOGI(TAG, "keep-awake active: sleep cancelled");
    if (!fastWake_) manualSleep_ = false;
    sleepPending_ = false;
    sleepIdleWait_.active = false;
    dashboard_.setMains(dashboardLive());
    return;
  }
  if (admin::ProvisionPortal::instance().apActive() ||
      eego::ble::BleManager::instance().sleepBlocked()) {
    ESP_LOGW(TAG, "recovery AP / BLE pairing active, sleep postponed");
    if (!fastWake_) manualSleep_ = false;
    sleepPending_ = false;
    sleepIdleWait_.active = false;
    lastScheduleCheckMs_ = now + 30000;
    dashboard_.setMains(dashboardLive());
    return;
  }
  const int idle = pollNetIdle(sleepIdleWait_, now);
  if (idle == 0) return;
  if (idle < 0) ESP_LOGW(TAG, "net not idle within budget; sleeping anyway");
  sleepNow();
}

bool PanelApp::batchSucceeded() const {
  if (!wifiSeen_) return false;
  const int64_t since_ms = static_cast<int64_t>(wakeStartUnix_) * 1000;
  size_t enabled = 0;
  for (const DashboardSourceInfo& source : dashboard_.sources()) {
    if (!source.enabled) continue;
    ++enabled;
    if (source.last_success_ms > 0 && source.last_success_ms >= since_ms) return true;
  }
  return enabled == 0;
}

void PanelApp::sleepNow() {
  battery::RtcState& rtc = battery::rtcState();
  charging_ = eego::PowerManager::instance().isCharging();
  batCache_ = eego::PowerManager::instance().readBattery();
  const uint64_t now_unix = TimeSource::valid() ? TimeSource::now() : 0;

  if (!mains_ && batchStarted_) {
    if (batchSucceeded()) {
      rtc.consecutive_failures = 0;
      rtc.last_data_unix = now_unix;
    } else if (rtc.consecutive_failures < 16) {
      ++rtc.consecutive_failures;
    }
  }
  const battery::WakePlan plan = battery::planNextWake(wakeInputs(now_unix));

  if (!fastWake_ && eego::ble::BleManager::instance().beforeSleep() != ESP_OK) {
    ESP_LOGW(TAG, "BLE stop failed; deep sleep powers the radio down anyway");
  }
  eego::FrontlightManager::instance().off();
  const eego::net::ApHint ap = NetStack::instance().wifi().lastConnectedAp();
  if (ap.valid) {
    rtc.ap_valid = 1;
    rtc.ap_channel = ap.channel;
    memcpy(rtc.ap_bssid, ap.bssid, sizeof(rtc.ap_bssid));
    snprintf(rtc.ap_ssid, sizeof(rtc.ap_ssid), "%s", ap.ssid.c_str());
  } else if (fastWake_) {
    rtc.ap_valid = 0;
  }
  NetStack::instance().wifi().powerOff();

  presentSleepFrame(plan, now_unix);

  rtc.last_wake_unix = now_unix;
  ++rtc.wakes;
  rtc.manual_sleep = manualSleep_ ? 1 : 0;
  rtc.charge_suspect = mainsDetector_.chargeSuspect() ? 1 : 0;
  battery::commitRtcState();
  if (deps_.touch != nullptr) deps_.touch->sleep();
  if (deps_.boot.displayReady && deps_.display != nullptr) deps_.display->deepSleep();

  ESP_LOGI(TAG, "deep sleep: kind=%d interval=%umin wake=%llu failures=%u",
           static_cast<int>(plan.kind), static_cast<unsigned>(plan.interval_min),
           static_cast<unsigned long long>(plan.wake_unix),
           static_cast<unsigned>(rtc.consecutive_failures));
  battery::deepSleep(plan.sleep_s, plan.kind != battery::WakeKind::CriticalBattery);
}

void PanelApp::sleepByPowerKey(int64_t now) {
  if (admin::ProvisionPortal::instance().apActive() ||
      eego::ble::BleManager::instance().sleepBlocked()) {
    showBanner(tr(Str::SleepBlocked), 3000);
    return;
  }
  ESP_LOGI(TAG, "power key: sleep (%s)", mains_ ? "mains" : "battery");
  clearConfirm();
  if (keepAwakeUntil(now) != 0) admin::setKeepAwakeMinutes(0);
  manualSleep_ = true;
  requestSleep(now, "power key");
}

void PanelApp::armShutdown() {
  if (page_ == Page::Provision) return;
  activeInputControl_ = "trigger:power";
  armConfirm(ConfirmKind::Shutdown);
}

void PanelApp::shutdownNow() {
  ESP_LOGI(TAG, "power key: shutting down");
  if (eego::ble::BleManager::instance().beforeSleep() != ESP_OK) {
    ESP_LOGW(TAG, "BLE stop failed; power-off cuts the radio anyway");
  }
  eego::FrontlightManager::instance().off();
  NetStack::instance().wifi().powerOff();

  battery::RtcState& rtc = battery::rtcState();
  rtc.manual_sleep = 0;
  rtc.frame.valid = 0;
  battery::commitRtcState();
  if (deps_.touch != nullptr) deps_.touch->sleep();

  if (deps_.boot.displayReady && deps_.canvas != nullptr && deps_.display != nullptr) {
    eego::PortraitCanvas& c = *deps_.canvas;
    drawPoweredOff(c);
    c.resetClip();
    c.requestFullRefresh();
    c.display(eego::RefreshMode::Full);
    c.waitRefreshComplete();
    deps_.display->deepSleep();
  }
  battery::powerOff();
}

uint64_t PanelApp::frameHash() const {
  const eego::Uc8279cDisplay& d = *deps_.display;
  uint64_t h = battery::fnv1a64(d.framebuffer(), eego::FRAMEBUFFER_SIZE);
  if (d.hasGrayContent() && d.grayMsbPlane() != nullptr && d.grayLsbPlane() != nullptr) {
    h = battery::fnv1a64(d.grayMsbPlane(), eego::FRAMEBUFFER_SIZE, h);
    h = battery::fnv1a64(d.grayLsbPlane(), eego::FRAMEBUFFER_SIZE, h);
  }
  return h;
}

DeviceStateInfo PanelApp::sleepingState(uint64_t stamp, uint32_t interval_min) const {
  DeviceStateInfo info;
  info.state = DeviceState::Sleeping;
  if (stamp >= battery::kMinValidUnix) {
    battery::NightWindow night{settings_.sleep_start_min, settings_.sleep_end_min};
    info.next_unix = battery::refreshDeadline(stamp, settings_.battery_max_stale_min,
                                              settings_.timezone_offset_min, interval_min, night);
  }
  return info;
}

void PanelApp::drawNotice(const char* title, const char* line1, const char* line2,
                          const char* line3) {
  eego::PortraitCanvas& c = *deps_.canvas;
  c.clear();
  c.useUiContentRect();
  const int16_t titleLh = lineHeight(style::kTitle.scale);
  const int16_t captionLh = lineHeight(style::kCaption.scale);
  const int16_t blockH = static_cast<int16_t>(titleLh + 8 + 3 * captionLh);
  int16_t y = static_cast<int16_t>((geom::kContentTop + geom::kContentBottom - blockH) / 2);
  drawTextFit(c, geom::kInnerX, y, geom::kInnerW, title, style::kTitle.scale, style::kTitle.bold,
              style::kTitle.gray);
  y = static_cast<int16_t>(y + titleLh + 8);
  for (const char* line : {line1, line2, line3}) {
    if (line != nullptr && line[0] != '\0') {
      drawTextFit(c, geom::kInnerX, y, geom::kInnerW, line, style::kCaption.scale,
                  style::kCaption.bold, style::kCaption.gray);
    }
    y = static_cast<int16_t>(y + captionLh);
  }
  c.resetClip();
}

RTC_DATA_ATTR static uint32_t s_rotateScreen = 0;
RTC_DATA_ATTR static uint64_t s_rotateEpoch = 0;

void PanelApp::rotateForSleepFrame(uint64_t slotUnix) {
  const size_t count = dashboard_.screenCount();
  if (settings_.screen_rotate_min == 0 || count < 2) {
    s_rotateScreen = 0;
    s_rotateEpoch = 0;
    return;
  }
  const uint64_t epoch = screenRotateEpoch(slotUnix, nowMs(), settings_.timezone_offset_min,
                                           settings_.screen_rotate_min);
  uint32_t index = s_rotateScreen;
  if (s_rotateEpoch != 0 && epoch != s_rotateEpoch) ++index;
  s_rotateEpoch = epoch;
  s_rotateScreen = static_cast<uint32_t>(index % count);
  dashboard_.showScreen(s_rotateScreen);
}

void PanelApp::serviceScreens(int64_t now) {
  uint32_t requested = 0;
  if (admin::consumeScreenRequest(requested) && dashboard_.available() &&
      battPhase_ != BattPhase::Fetching) {
    dashboard_.showScreen(requested);
    if (page_ != Page::Dashboard) gotoPage(Page::Dashboard);
    noteInteraction(now);
  }

  const uint32_t period = settings_.screen_rotate_min;
  if (period == 0 || page_ != Page::Dashboard || dashboard_.screenCount() < 2 ||
      battPhase_ == BattPhase::Fetching) {
    rotateEpoch_ = 0;
  } else {
    const uint64_t epoch =
        screenRotateEpoch(nowUnix(), now, settings_.timezone_offset_min, period);
    if (rotateEpoch_ == 0) {
      rotateEpoch_ = epoch;
    } else if (screenRotateDue(epoch, rotateEpoch_, now, refreshPolicy_.lastInteractionMs(),
                               period, rotateShown_)) {
      rotateEpoch_ = epoch;
      rotateShown_ = false;
      dashboard_.moveScreen(1);
      s_rotateScreen = static_cast<uint32_t>(dashboard_.screenIndex());
    } else if (epoch != rotateEpoch_ &&
               now - refreshPolicy_.lastInteractionMs() <
                   static_cast<int64_t>(period) * 60000) {
      rotateEpoch_ = epoch;
    }
  }

  if (now - lastScreenPublishMs_ >= 500) {
    lastScreenPublishMs_ = now;
    admin::ScreenState state;
    state.available = dashboard_.available();
    state.index = static_cast<uint32_t>(dashboard_.screenIndex());
    state.count = static_cast<uint32_t>(dashboard_.screenCount());
    state.page_id = dashboard_.screenPageId();
    state.shown = page_ == Page::Dashboard;
    state.rotate_min = period;
    admin::publishScreenState(state);
  }
}

void PanelApp::presentSleepFrame(const battery::WakePlan& plan, uint64_t now_unix) {
  if (!deps_.boot.displayReady || deps_.canvas == nullptr || deps_.display == nullptr) return;
  battery::RtcState& rtc = battery::rtcState();
  battery::FrameRecord& rec = rtc.frame;
  const bool critical = plan.kind == battery::WakeKind::CriticalBattery;
  const bool night = plan.kind == battery::WakeKind::NightPause;
  const bool panel_known = fastWake_ && rec.valid != 0;
  const uint32_t interval = plan.interval_min != 0 ? plan.interval_min : settings_.battery_refresh_min;
  const uint8_t bucket = battery::batteryDisplayBucket(batCache_.percent, rtc.battery_bucket);
  const uint64_t slot =
      now_unix < battery::kMinValidUnix ? 0
      : fastWake_ ? battery::nominalSlot(now_unix, settings_.timezone_offset_min, interval)
                  : now_unix - now_unix % 60;

  bool skip = false;
  uint64_t hash = 0;
  if (critical) {
    char title[48];
    char last[64];
    char when[24];
    snprintf(title, sizeof(title), tr(Str::LowBatteryTitle),
             static_cast<unsigned>(batCache_.percent));
    formatTs(rec.refresh_unix, when, sizeof(when));
    snprintf(last, sizeof(last), tr(Str::LastDataFmt), when);
    drawNotice(title, tr(Str::LowBatteryHint), last, tr(Str::WakeHint));
    hash = frameHash();
    skip = panel_known && rtc.low_battery_shown != 0 && hash == rec.hash;
  } else if (dashboard_.available()) {
    if (!fastWake_) {
      const bool keep_screen = manualSleep_ && page_ == Page::Dashboard;
      page_ = Page::Dashboard;
      if (!keep_screen) dashboard_.home();
      deps_.canvas->clear();
      dashboard_.invalidateAll();
    }
    rotateForSleepFrame(slot);
    if (night) {
      DeviceStateInfo info;
      info.state = DeviceState::NightPause;
      info.next_unix = plan.wake_unix;
      dashboard_.setDeviceState(info, settings_.timezone_offset_min);
      dashboard_.setSleepFrame(true, slot, mains_ ? -1 : static_cast<int>(bucket));
      dashboard_.renderOffscreen();
      hash = frameHash();
      skip = panel_known && hash == rec.hash;
    } else if (mains_ && !manualSleep_) {
      dashboard_.setSleepFrame(false);
      dashboard_.renderOffscreen();
      hash = frameHash();
    } else {
      if (panel_known) {
        dashboard_.setDeviceState(sleepingState(rec.refresh_unix, interval),
                                  settings_.timezone_offset_min);
        dashboard_.setSleepFrame(true, rec.refresh_unix, bucket);
        dashboard_.renderOffscreen();
        hash = frameHash();
        skip = hash == rec.hash && !battery::frameStale(rec, now_unix, settings_.battery_max_stale_min);
      }
      if (!skip) {
        dashboard_.setDeviceState(sleepingState(slot, interval), settings_.timezone_offset_min);
        dashboard_.setSleepFrame(true, slot, bucket);
        dashboard_.renderOffscreen();
        hash = frameHash();
      }
    }
  } else {
    if (night) {
      deviceState_ = DeviceStateInfo{};
      deviceState_.state = DeviceState::NightPause;
      deviceState_.next_unix = plan.wake_unix;
    } else {
      deviceState_ = sleepingState(slot, interval);
    }
    banner_.clear();
    eego::PortraitCanvas& c = *deps_.canvas;
    c.clear();
    c.useUiContentRect();
    StatusBarModel sb;
    buildStatusBar(sb);
    drawStatusBar(c, sb);
    OnboardingModel m;
    buildOnboarding(m);
    m.asleep = true;
    drawOnboarding(c, m);
    c.resetClip();
    hash = frameHash();
    skip = panel_known && hash == rec.hash;
  }
  if (skip) {
    ESP_LOGI(TAG, "sleep frame unchanged: panel untouched");
    return;
  }

  uint8_t* previous = nullptr;
  bool previous_loaded = false;
  if (fastWake_) {
    if (rec.valid != 0) {
      previous = static_cast<uint8_t*>(
          heap_caps_malloc(eego::FRAMEBUFFER_SIZE, MALLOC_CAP_SPIRAM));
      previous_loaded = previous != nullptr &&
                        battery::loadStoredFrame(previous, eego::FRAMEBUFFER_SIZE, rec.hash);
    }
  } else {
    previous_loaded = displayedThisBoot_;
  }
  const battery::RefreshAction action =
      battery::refreshMode(rec, now_unix, previous_loaded, critical);
  if (action == battery::RefreshAction::Fast && previous_loaded && previous != nullptr) {
    deps_.display->setPreviousFrame(previous);
  }
  const bool full = action == battery::RefreshAction::Full;
  ESP_LOGI(TAG, "sleep frame: %s refresh (fast_since_full=%u, previous=%d)",
           full ? "full" : "fast", static_cast<unsigned>(rec.fast_since_full),
           previous_loaded ? 1 : 0);
  if (full) deps_.canvas->requestFullRefresh();
  deps_.canvas->display(full ? eego::RefreshMode::Full : eego::RefreshMode::Fast);
  deps_.canvas->waitRefreshComplete();
  free(previous);

  battery::noteRefreshed(rec, action, hash, slot, now_unix);
  rtc.battery_bucket = static_cast<int16_t>(bucket);
  rtc.low_battery_shown = critical ? 1 : 0;
  if (!battery::storeFrame(deps_.display->framebuffer(), eego::FRAMEBUFFER_SIZE, hash)) {
    ESP_LOGW(TAG, "frame not stored: next wake uses a full refresh");
  }
}

void PanelApp::enterProvision() {
  provisionSucceeded_ = false;
  page_ = Page::Provision;
  clearConfirm();
  markDirty(true);
}

void PanelApp::leaveProvision() {
  provisionSucceeded_ = false;
  gotoPage(home());
}

int64_t PanelApp::keepAwakeUntil(int64_t now) const {
  const int64_t until = admin::keepAwakeUntilMs();
  return admin::keep_awake::active(now, until) ? until : 0;
}

void PanelApp::serviceKeepAwake(int64_t now) {
  const int64_t until = keepAwakeUntil(now);
  const bool active = until != 0;
  const uint32_t revision = admin::keepAwakeRevision();
  if (revision != keepAwakeRevision_) {
    keepAwakeRevision_ = revision;
    if (active) noteInteraction(now);
    if (page_ == Page::Settings || page_ == Page::Status) markDirty();
  }
  if (active != keepAwakeWasActive_) {
    keepAwakeWasActive_ = active;
    ESP_LOGI(TAG, "keep-awake %s", active ? "on" : "off (expired/cancelled)");
    if (!active && admin::keepAwakeUntilMs() != 0) admin::setKeepAwakeMinutes(0);
    applyWifiPowerSave();
    if (page_ == Page::Settings || page_ == Page::Status) markDirty();
  }
  refreshPolicy_.setHoldUntil(until);
}

void PanelApp::toggleKeepAwake() {
  const int64_t now = nowMs();
  const uint32_t left = admin::keep_awake::remainingS(now, keepAwakeUntil(now));
  const uint32_t duration = settings_.keep_awake_min;
  const uint32_t minutes = admin::keep_awake::toggleMinutes(left, duration);
  admin::setKeepAwakeMinutes(minutes);
  serviceKeepAwake(now);
  noteInteraction(now);
  if (minutes == 0) {
    showBanner(tr(Str::KeepAwakeCancelled), 3000);
  } else {
    showBannerFmt(3000, tr(left == 0 ? Str::KeepAwakeStartedFmt : Str::KeepAwakeExtendedFmt),
                  static_cast<unsigned>(minutes));
  }
  syncDeviceState(now, true);
  markDirty();
}

DeviceStateInfo PanelApp::computeDeviceState(int64_t now, uint64_t ref_unix) {
  DeviceStateInfo info;
  info.idle_refresh_min = settings_.idle_refresh_min;
  const int64_t until = keepAwakeUntil(now);
  if (until != 0) {
    info.state = DeviceState::KeepAwake;
    info.keep_awake_min =
        admin::keep_awake::displayMinutes(admin::keep_awake::remainingS(now, until));
    return info;
  }
  if (!mains_ && battPhase_ == BattPhase::Interactive) {
    info.state = DeviceState::Awake;
    info.awake_timeout_s = settings_.battery_interactive_timeout_s;
    return info;
  }
  if (mains_ && refreshPolicy_.idle(now)) {
    info.state = DeviceState::Idle;
    if (ref_unix >= RefreshPolicy::kMinValidUnix) {
      info.next_unix = nextRefreshBoundary(ref_unix, settings_.timezone_offset_min,
                                           settings_.idle_refresh_min);
    }
    return info;
  }
  return info;
}

void PanelApp::syncDeviceState(int64_t now, bool before_render) {
  lastDeviceStateMs_ = now;
  const uint64_t unix_now = TimeSource::valid() ? TimeSource::now() : 0;
  const uint64_t ref = before_render || lastDisplayedUnix_ == 0 ? unix_now : lastDisplayedUnix_;
  const DeviceStateInfo next = computeDeviceState(now, ref);
  if (next == deviceState_) return;
  deviceState_ = next;
  dashboard_.setDeviceState(deviceState_, settings_.timezone_offset_min);
  if (page_ != Page::Dashboard && !before_render) markDirty();
}

PowerDetail PanelApp::powerDetail() const {
  switch (mainsDetector_.reason()) {
    case battery::PowerReason::ForcedMains:
    case battery::PowerReason::ForcedBattery: return PowerDetail::Forced;
    case battery::PowerReason::Charging: return PowerDetail::Charging;
    case battery::PowerReason::UsbHost: return PowerDetail::UsbHost;
    case battery::PowerReason::FullHold: return PowerDetail::Full;
    default: return PowerDetail::None;
  }
}

void PanelApp::publishPowerState(int64_t now) {
  lastPowerPublishMs_ = now;
  admin::PowerSnapshot p;
  p.mains = mains_;
  p.source = mains_ ? "mains" : "battery";
  p.source_reason = battery::powerReasonName(mainsDetector_.reason());
  p.power_mode = eego::settings::powerModeName(settings_.power_mode);
  p.mode = deviceStateApiName(deviceState_.state);
  p.battery_mv = batCache_.millivolts;
  p.battery_pct = batCache_.millivolts > 0 ? batCache_.percent : -1;
  p.charging = chargingShown();
  p.usb_host = usbHost_;
  p.next_wake = plannedWakeAt();
  p.next_refresh = deviceState_.state == DeviceState::Idle ? deviceState_.next_unix : 0;
  if (!mains_ && battPhase_ == BattPhase::Interactive && keepAwakeUntil(now) == 0) {
    const int64_t last =
        std::max(refreshPolicy_.lastInteractionMs(), admin::lastAuthenticatedRequestMs());
    const int64_t left =
        static_cast<int64_t>(settings_.battery_interactive_timeout_s) * 1000 - (now - last);
    p.sleep_in_s = std::max<int64_t>(0, (left + 999) / 1000);
  }
  p.last_wake_reason = battery::wakeSourceName(wakeSource_);
  p.sleeps_since_boot = battery::rtcState().wakes;
  p.last_screen_update = lastDisplayedUnix_;
  admin::publishPowerSnapshot(p);
}

void PanelApp::refreshBattery(int64_t now) {
  const int64_t period =
      mainsDetector_.wantsFastSamples() ? battery::kFastBatterySampleMs : 60000;
  if (now - lastBatMs_ < period) return;
  lastBatMs_ = now;
  batCache_ = eego::PowerManager::instance().readBattery();
}

const eego::net::NetStatusSnapshot& PanelApp::netStatus() {
  const int64_t now = nowMs();
  if (now - netStatusCacheMs_ > 5000) {
    eego::net::NetStatusSnapshot snap;
    if (NetStack::instance().queryStatus(snap, 300)) {
      netStatusCache_ = std::move(snap);
      wifiState_.store(netStatusCache_.wifi.state);
    }
    netStatusCacheMs_ = now;
  }
  return netStatusCache_;
}

uint64_t PanelApp::nowUnix() {
  return TimeSource::now();
}

const alert_core::AlertRecord* PanelApp::findRecord(const std::string& id) const {
  if (snapshot_ == nullptr) return nullptr;
  for (const alert_core::AlertRecord& r : snapshot_->records) {
    if (r.id == id) return &r;
  }
  return nullptr;
}

void PanelApp::formatTs(uint64_t ts, char* out, size_t cap) const {
  if (ts == 0) {
    snprintf(out, cap, "--");
    return;
  }
  struct tm tm_buf;
  localTm(settings_, ts, tm_buf);
  snprintf(out, cap, "%02d-%02d %02d:%02d", tm_buf.tm_mon + 1, tm_buf.tm_mday, tm_buf.tm_hour,
           tm_buf.tm_min);
}

void PanelApp::formatAge(uint64_t ts, char* out, size_t cap) {
  if (ts == 0) {
    snprintf(out, cap, "%s", tr(Str::Never));
    return;
  }
  const uint64_t now = nowUnix();
  if (now <= ts) {
    snprintf(out, cap, "%s", tr(Str::AgeNow));
    return;
  }
  const uint64_t diff = now - ts;
  if (diff < 60) {
    snprintf(out, cap, "%s", tr(Str::AgeNow));
  } else if (diff < 3600) {
    snprintf(out, cap, tr(Str::AgeMinFmt), static_cast<unsigned>(diff / 60));
  } else if (diff < 172800) {
    snprintf(out, cap, tr(Str::AgeHourFmt), static_cast<unsigned>(diff / 3600));
  } else {
    snprintf(out, cap, tr(Str::AgeDayFmt), static_cast<unsigned>(diff / 86400));
  }
}

const char* PanelApp::typeName(const std::string& typeId, std::string& buf) const {
  if (deps_.registry == nullptr) {
    buf = typeId;
    return buf.c_str();
  }
  const alert_core::AlertTypeDescriptor d = deps_.registry->resolve(typeId);
  buf = (currentLang() == Lang::Zh) ? d.name_zh : d.name_en;
  return buf.c_str();
}

size_t PanelApp::softKeyCount() const {
  switch (page_) {
    case Page::Onboarding: return 2;
    case Page::AlertDetail: {
      const DashboardAlertItem* item = detailItem();
      if (item == nullptr) return 1;
      return item->linked_panel_id.empty() ? 4 : 5;
    }
    case Page::Status: return 3;
    case Page::Settings: return 3;
    case Page::Help: return 1;
    default: return 0;
  }
}

void PanelApp::softKeys(SoftKeyModel* out, size_t cap) const {
  const size_t n = softKeyCount();
  if (n == 0 || cap < n) return;
  switch (page_) {
    case Page::Onboarding:
      out[0] = {tr(Str::StatusKey), true};
      out[1] = {tr(Str::SettingsKey), true};
      break;
    case Page::AlertDetail: {
      const DashboardAlertItem* item = detailItem();
      if (item == nullptr) {
        out[0] = {tr(Str::Back), true};
        break;
      }
      const alert_core::AlertRecord* rec = detailRecord();
      const bool clearable = rec != nullptr && (rec->state == alert_core::AlertState::Active ||
                                                rec->state == alert_core::AlertState::Acked);
      out[0] = {tr(Str::Ack), item->active && !item->acknowledged};
      out[1] = {tr(Str::Mute), rec != nullptr && !rec->muted};
      out[2] = {tr(Str::Clear), clearable};
      if (n == 5) out[3] = {tr(Str::Trend), true};
      out[n - 1] = {tr(Str::Back), true};
      break;
    }
    case Page::Status:
      out[0] = {tr(Str::Refresh), dashboard_.available()};
      out[1] = {tr(Str::KeepAwakeKey), true};
      out[2] = {tr(Str::Back), true};
      break;
    case Page::Settings:
      out[0] = {tr(Str::PrevPage), !pageEdge(-1)};
      out[1] = {tr(Str::Back), true};
      out[2] = {tr(Str::NextPage), !pageEdge(1)};
      break;
    case Page::Help:
      out[0] = {tr(Str::Back), true};
      break;
    default:
      break;
  }
}

void PanelApp::openDetail(const std::string& panel_id, const std::string& identity) {
  detailPanel_ = panel_id;
  detailIdentity_ = identity;
  detailIndex_ = 0;
  refreshDetailItems();
  gotoPage(Page::AlertDetail);
  detailRendered_.clear();
  lastDetailCheckMs_ = nowMs();
}

void PanelApp::refreshDetailItems() {
  detailItems_ = dashboard_.alertItems(detailPanel_);
  if (detailItems_.empty()) {
    detailIndex_ = 0;
    return;
  }
  for (size_t i = 0; i < detailItems_.size(); ++i) {
    if (detailItems_[i].identity == detailIdentity_) {
      detailIndex_ = i;
      return;
    }
  }
  detailIndex_ = std::min(detailIndex_, detailItems_.size() - 1);
  detailIdentity_ = detailItems_[detailIndex_].identity;
}

const DashboardAlertItem* PanelApp::detailItem() const {
  if (detailItems_.empty() || detailIndex_ >= detailItems_.size()) return nullptr;
  return &detailItems_[detailIndex_];
}

const alert_core::AlertRecord* PanelApp::detailRecord() const {
  const DashboardAlertItem* item = detailItem();
  if (item == nullptr || item->core_id.empty()) return nullptr;
  return findRecord(item->core_id);
}

void PanelApp::moveDetail(int delta) {
  if (detailItems_.empty() || delta == 0) return;
  const int64_t next = std::max<int64_t>(
      0, std::min<int64_t>(static_cast<int64_t>(detailIndex_) + delta,
                           static_cast<int64_t>(detailItems_.size()) - 1));
  if (static_cast<size_t>(next) == detailIndex_) return;
  detailIndex_ = static_cast<size_t>(next);
  detailIdentity_ = detailItems_[detailIndex_].identity;
  clearConfirm();
  ++pageEpoch_;
  focus_ = 0;
  markDirty();
}

std::string PanelApp::detailSignature() const {
  const DashboardAlertItem* item = detailItem();
  if (item == nullptr) return "empty";
  std::string out = item->identity;
  out += '|';
  out += std::to_string(detailItems_.size());
  out += item->active ? "A" : "C";
  out += item->acknowledged ? "K" : "-";
  out += std::to_string(item->severity);
  out += item->title;
  out += item->message;
  if (const alert_core::AlertRecord* rec = detailRecord()) {
    out += std::to_string(static_cast<unsigned>(rec->state));
    out += rec->muted ? "M" : "-";
    out += std::to_string(rec->occurrences);
  }
  return out;
}

void PanelApp::render(int64_t now) {
  if (page_ == Page::Dashboard) {
    dashboard_.render();
    noteInputResume();
    forceFull_ = false;
    dirty_ = false;
    lastRenderMs_ = now;
    return;
  }
  if (deps_.publisher != nullptr) {
    auto snap = deps_.publisher->acquire();
    if (snap) {
      snapshot_ = snap;
      snapshotSeq_ = snap->seq;
    }
  }

  eego::PortraitCanvas& c = *deps_.canvas;
  c.clear();
  c.useUiContentRect();

  StatusBarModel sb;
  buildStatusBar(sb);
  drawStatusBar(c, sb);

  switch (page_) {
    case Page::Onboarding: {
      OnboardingModel m;
      buildOnboarding(m);
      onboardingKey_ = m.url + "|" + m.network + "|" + (m.apMode ? "ap" : "sta") + "|" + m.reason;
      lastOnboardingCheckMs_ = now;
      drawOnboarding(c, m);
      break;
    }
    case Page::AlertDetail: {
      refreshDetailItems();
      AlertDetailModel m;
      buildDetail(m);
      detailRendered_ = detailSignature();
      drawAlertDetail(c, m);
      break;
    }
    case Page::Status: {
      KvModel m;
      buildStatus(m);
      lastStatusRenderMs_ = now;
      drawStatus(c, m);
      break;
    }
    case Page::Settings: {
      S4Model m;
      buildS4(m);
      drawS4(c, m);
      break;
    }
    case Page::Help: {
      KvModel m;
      buildHelp(m);
      drawHelp(c, m);
      break;
    }
    case Page::Provision: {
      ProvisionModel m;
      m.succeeded = provisionSucceeded_;
      const admin::ProvisionPortal& portal = admin::ProvisionPortal::instance();
      snprintf(m.windowText, sizeof(m.windowText), "%s", tr(Str::ProvIdleClose));
      const std::string ssid = portal.provSsidCopy();
      switch (portal.provState()) {
        case admin::ProvisionPortal::ProvState::Connecting:
          snprintf(m.statusText, sizeof(m.statusText), tr(Str::ProvConnectingFmt), ssid.c_str());
          break;
        case admin::ProvisionPortal::ProvState::Fail: {
          const int code = portal.provReasonCode();
          snprintf(m.statusText, sizeof(m.statusText), tr(Str::ProvFailedFmt),
                   code < 0 ? tr(Str::ProvTimeout)
                            : eego::net::wifiReasonShort(static_cast<uint8_t>(code),
                                                         currentLang() == Lang::En));
          break;
        }
        case admin::ProvisionPortal::ProvState::Ok: {
          const std::string ip = portal.provIpCopy();
          if (!ip.empty()) {
            snprintf(m.statusText, sizeof(m.statusText), tr(Str::ProvNextFmt), ssid.c_str(),
                     ip.c_str());
          }
          break;
        }
        default: break;
      }
      drawProvision(c, m);
      break;
    }
    case Page::BlePair: {
      const eego::ble::Status ble = eego::ble::BleManager::instance().status();
      BlePairModel m;
      snprintf(m.passkey, sizeof(m.passkey), "%06lu",
               static_cast<unsigned long>(ble.pairing_passkey % 1000000u));
      snprintf(m.deviceName, sizeof(m.deviceName), "%s", ble.device_name.c_str());
      m.remainingS = ble.pairing_remaining_s;
      drawBlePair(c, m);
      break;
    }
    case Page::Dashboard:
      break;
  }

  SoftKeyModel keys[5];
  const size_t n = softKeyCount();
  softKeys(keys, 5);
  const size_t focusIdx = page_ == Page::Settings ? static_cast<size_t>(-1) : focus_;
  drawSoftKeys(c, keys, n, focusIdx);
  drawTouchMark(c);

  c.display(forceFull_ ? eego::RefreshMode::Full : eego::RefreshMode::Fast);
  c.waitRefreshComplete();
  noteInputResume();
  if (forceFull_) lastFullRenderMs_ = now;
  forceFull_ = false;
  dirty_ = false;
  lastRenderMs_ = now;
}

void PanelApp::buildStatusBar(StatusBarModel& m) {
  renderedClockKey_ = -1;
  lastClockCheckMs_ = nowMs();
  if (TimeSource::valid()) {
    const uint64_t now = TimeSource::now();
    renderedClockKey_ = static_cast<int64_t>(now / 60);
    struct tm tm_buf;
    localTm(settings_, now, tm_buf);
    snprintf(m.timeText, sizeof(m.timeText), "%02d:%02d", tm_buf.tm_hour, tm_buf.tm_min);
  }
  m.state = deviceState_.state;
  const std::vector<std::string> labels =
      deviceStateLabels(deviceState_, settings_.timezone_offset_min);
  snprintf(m.stateText, sizeof(m.stateText), "%s", labels.front().c_str());
  if (labels.size() > 1) snprintf(m.stateShort, sizeof(m.stateShort), "%s", labels[1].c_str());
  m.batPercent = batCache_.percent;
  m.batMv = batCache_.millivolts;
  m.charging = chargingShown();
  m.wifiState = wifiState_.load();
  m.mains = mains_;
  m.portalOpen = admin::ProvisionPortal::instance().isOpen();
  m.apFallback = admin::ProvisionPortal::instance().isApFallbackActive();
  m.bleLinked = bleLinkSeen_ == eego::ble::LinkState::Authenticated;
  m.agentUplink = agentSeen_ && settings_.uplink != eego::settings::UplinkMode::Wifi;
  m.mutedCount = 0;
  if (snapshot_ != nullptr) {
    for (const auto& r : snapshot_->records) {
      if (r.muted) m.mutedCount++;
    }
  }
  if (!banner_.empty()) {
    snprintf(m.banner, sizeof(m.banner), "%s", banner_.c_str());
  }
}

namespace {
const char* wifiStateText(int state) {
  switch (state) {
    case 1: return tr(Str::WifiConnecting);
    case 2: return tr(Str::WifiConnected);
    case 3: return tr(Str::WifiFailed);
    case 4: return tr(Str::WifiOffline);
    default: return tr(Str::WifiDisconnected);
  }
}

void formatBytes(size_t bytes, char* out, size_t cap) {
  if (bytes >= 1024 * 1024) {
    snprintf(out, cap, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024) {
    snprintf(out, cap, "%u KB", static_cast<unsigned>((bytes + 512) / 1024));
  } else {
    snprintf(out, cap, "%u B", static_cast<unsigned>(bytes));
  }
}

void formatUptime(int64_t seconds, char* out, size_t cap) {
  const long long d = seconds / 86400;
  const long long h = (seconds % 86400) / 3600;
  const long long m = (seconds % 3600) / 60;
  if (d > 0) snprintf(out, cap, "%lldd %lldh %lldm", d, h, m);
  else if (h > 0) snprintf(out, cap, "%lldh %lldm", h, m);
  else snprintf(out, cap, "%lldm %llds", m, static_cast<long long>(seconds % 60));
}

}  // namespace

void PanelApp::buildOnboarding(OnboardingModel& m) {
  const eego::net::NetStatusSnapshot& ns = netStatus();
  const bool connected = ns.wifi.state == 2 && !ns.wifi.ip.empty();
  const bool ap = admin::ProvisionPortal::instance().apActive();
  char buf[96];
  if (connected) {
    m.url = "http://" + ns.wifi.ip + "/";
    snprintf(buf, sizeof(buf), "WiFi · %s · %d dBm", ns.wifi.ssid.c_str(), ns.wifi.rssi);
    m.network = buf;
  } else if (ap) {
    m.apMode = true;
    m.url = admin::ProvisionPortal::instance().isApFallbackActive() ? kWifiDiagUrl
                                                                     : admin::ProvisionPortal::kApUrl;
    snprintf(buf, sizeof(buf), "%s · %s", tr(Str::SetupHotspot), admin::ProvisionPortal::kApSsid);
    m.network = buf;
  } else if (ns.wifi.state == 4 || (ns.wifi.ssid.empty() && ns.wifi.state != 1)) {
    m.network = tr(Str::OnbNoWifi);
  } else {
    m.network = wifiStateText(ns.wifi.state);
    if (!ns.wifi.ssid.empty()) m.network += " · " + ns.wifi.ssid;
  }
  sanitizeClamped(m.network, 60);
  const auto document = admin::DashboardStore::instance().snapshot();
  if (document != nullptr && document->configured && !dashboard_.available()) {
    m.reason = dashboard_.unavailableReason();
    sanitizeClamped(m.reason, 80);
  }
}

void PanelApp::buildDetail(AlertDetailModel& m) {
  const DashboardAlertItem* item = detailItem();
  if (item == nullptr) {
    m.empty = true;
    return;
  }
  snprintf(m.position, sizeof(m.position), "%u/%u", static_cast<unsigned>(detailIndex_ + 1),
           static_cast<unsigned>(detailItems_.size()));
  m.severity = item->severity > 2 ? 2 : item->severity;
  const alert_core::AlertRecord* rec = detailRecord();
  if (rec != nullptr) {
    m.state = static_cast<uint8_t>(rec->state);
  } else {
    m.state = item->acknowledged ? 1 : (item->active ? 0 : 2);
  }
  std::string tname;
  if (rec != nullptr) {
    m.typeName = std::string(tr(Str::ExternalAlert)) + " · " + typeName(rec->type, tname);
  } else {
    m.typeName = tr(Str::RuleAlert);
    if (!item->rule_id.empty()) m.typeName += " · " + item->rule_id;
  }
  sanitizeClamped(m.typeName, 60);
  m.title = item->title;
  sanitizeClamped(m.title, 120);
  m.message = item->message;
  sanitizeClamped(m.message, 600);

  char buf[40];
  auto addMeta = [&m](Str label, std::string value) {
    sanitizeClamped(value, 80);
    m.meta.emplace_back(tr(label), std::move(value));
  };
  if (rec != nullptr) {
    addMeta(Str::LblSource, rec->source.id.empty() ? rec->source.kind : rec->source.id);
    formatTs(rec->first_seen, buf, sizeof(buf));
    addMeta(Str::LblFirstSeen, buf);
    formatTs(rec->last_seen, buf, sizeof(buf));
    addMeta(Str::LblLastSeen, buf);
    snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(rec->occurrences));
    addMeta(Str::LblOccurrences, buf);
    if (rec->expires_at != 0) {
      formatTs(rec->expires_at, buf, sizeof(buf));
      addMeta(Str::LblExpires, buf);
    } else {
      addMeta(Str::LblExpires, tr(Str::None));
    }
    addMeta(Str::LblAckBy, rec->ack_by.empty() ? tr(Str::Never) : rec->ack_by);
    if (rec->fields.type() == alert_core::JsonValue::Type::Object) {
      for (const auto& kv : rec->fields.objectItems()) {
        if (m.fields.size() >= 4) break;
        std::string key = kv.first;
        std::string val = kv.second.scalarText();
        sanitizeClamped(key, 32);
        sanitizeClamped(val, 64);
        m.fields.emplace_back(key, val);
      }
    }
  } else {
    addMeta(Str::LblSource, item->source_id);
    addMeta(Str::LblRule, item->rule_id);
    formatTs(item->timestamp_ms > 0 ? static_cast<uint64_t>(item->timestamp_ms / 1000) : 0, buf,
             sizeof(buf));
    addMeta(Str::LblTime, buf);
    formatAge(item->timestamp_ms > 0 ? static_cast<uint64_t>(item->timestamp_ms / 1000) : 0, buf,
              sizeof(buf));
    addMeta(Str::LblLastSeen, buf);
  }
}

size_t PanelApp::statusPageCount() const {
  const size_t per = geom::kStatusRowsPerPage;
  return std::max<size_t>(1, (statusRows_ + per - 1) / per);
}

void PanelApp::buildStatus(KvModel& m) {
  const eego::net::NetStatusSnapshot& ns = netStatus();
  m.configValid = admin::ConfigStore::instance().valid();
  std::vector<std::pair<std::string, std::string>> rows;
  auto add = [&rows](const std::string& label, std::string value) {
    sanitizeClamped(value, 80);
    if (value.empty()) value = "-";
    rows.emplace_back(label, std::move(value));
  };
  char buf[128];
  std::vector<bool> big;
  auto section = [&](Str label) {
    rows.emplace_back(tr(label), "");
    big.resize(rows.size(), false);
    big.back() = true;
  };
  auto addBigText = [&](const std::string& label, std::string value) {
    add(label, std::move(value));
    big.resize(rows.size(), false);
    big.back() = true;
  };
  auto addBig = [&](Str label, std::string value) { addBigText(tr(label), std::move(value)); };

  section(Str::SecPower);
  {
    const int64_t now = nowMs();
    const int64_t until = keepAwakeUntil(now);
    const bool interactive = !mains_ && battPhase_ == BattPhase::Interactive;
    PowerStatusInput in;
    in.external = mains_;
    in.detail = powerDetail();
    in.charging = chargingShown();
    in.battery_mv = batCache_.millivolts;
    in.battery_pct = batCache_.percent;
    in.state = deviceState_;
    in.auto_sleep = interactive && until == 0;
    in.sleep_timeout_s = settings_.battery_interactive_timeout_s;
    for (auto& row : powerStatusRows(in, settings_.timezone_offset_min)) {
      addBigText(row.first, std::move(row.second));
    }
    const uint64_t wake = plannedWakeAt();
    if (!interactive && wake != 0) {
      formatTs(wake, buf, sizeof(buf));
      addBig(Str::LblNextWake, buf);
    }
    addBig(Str::LblWakeReason, tr(wakeSourceText(wakeSource_)));
    snprintf(buf, sizeof(buf), tr(Str::SleepsFmt),
             static_cast<unsigned>(battery::rtcState().wakes));
    addBig(Str::LblSleeps, buf);
  }

  section(Str::SecNetwork);
  std::string wifi = wifiStateText(ns.wifi.state);
  if (wifi.rfind("WiFi ", 0) == 0) wifi.erase(0, 5);
  if (!ns.wifi.ssid.empty()) wifi += " · " + ns.wifi.ssid;
  addBig(Str::LblWifi, wifi);
  if (ns.wifi.state == 2) {
    const bool en = currentLang() == Lang::En;
    const eego::net::WifiSignal q = eego::net::wifiSignalQuality(ns.wifi.rssi);
    snprintf(buf, sizeof(buf), "%d dBm · %s", ns.wifi.rssi, eego::net::wifiSignalLabel(q, en));
    addBig(Str::LblSignal, buf);
    if (ns.wifi.rssi_samples >= 3) {
      snprintf(buf, sizeof(buf), en ? "MIN %d · AVG %d dBm" : "最低 %d · 平均 %d dBm",
               ns.wifi.rssi_min, ns.wifi.rssi_avg);
      addBig(Str::LblSignal10m, buf);
    }
    if (q == eego::net::WifiSignal::Weak) addBig(Str::LblTip, tr(Str::WifiWeakHint));
  }
  if (!ns.wifi.ip.empty()) {
    addBig(Str::LblAddress, "http://" + ns.wifi.ip + "/");
  }
  if (ns.wifi.state != 2) {
    const bool en = currentLang() == Lang::En;
    const eego::net::WifiDiagSnapshot diag = eego::net::WifiManager::instance().diagnostics();
    for (const eego::net::WifiNetworkDiag& n : diag.networks) {
      std::string value;
      if (n.result == eego::net::WifiAttemptResult::None) {
        value = tr(Str::WifiNotTried);
      } else if (n.result == eego::net::WifiAttemptResult::Connecting) {
        value = tr(Str::WifiConnecting);
      } else if (n.failures > 0) {
        value = eego::net::wifiReasonShort(n.last_reason, en);
      } else {
        value = wifiStateText(static_cast<int>(eego::net::WifiState::Connected));
      }
      if (!n.seen && diag.scan_ms >= 0) value += std::string(" · ") + tr(Str::WifiNotSeen);
      std::string label = n.ssid;
      sanitizeClamped(label, 10);
      addBigText(label, value);
    }
  }
  if (admin::ProvisionPortal::instance().apActive()) {
    addBig(Str::SetupHotspot, admin::ProvisionPortal::kApSsid);
    addBig(Str::WifiDiag, admin::ProvisionPortal::instance().isApFallbackActive()
                              ? kWifiDiagUrl
                              : admin::ProvisionPortal::kApUrl);
  }
  if (ns.time.valid) {
    if (ns.time.sntp_synced) {
      char sync_at[40];
      formatTs(ns.time.last_sync_at, sync_at, sizeof(sync_at));
      snprintf(buf, sizeof(buf), "%s · %s", tr(Str::TimeNetwork), sync_at);
    } else {
      snprintf(buf, sizeof(buf), "%s", tr(Str::TimeRtc));
    }
  } else {
    snprintf(buf, sizeof(buf), "%s", tr(Str::TimeUnavailable));
  }
  addBig(Str::TimeLabel, buf);

  section(Str::SecDevice);
  if (eego::SdCard::instance().ready()) {
    std::string storage = tr(Str::SdReady);
    if (ns.alert_db.attached) {
      char size[24];
      formatBytes(ns.alert_db.file_bytes, size, sizeof(size));
      storage += std::string(" · alerts.db ") + size;
      if (ns.alert_db.io_failures > 0) {
        snprintf(buf, sizeof(buf), " · IO!%u", static_cast<unsigned>(ns.alert_db.io_failures));
        storage += buf;
      }
    }
    add(tr(Str::LblStorage), storage);
  } else {
    add(tr(Str::LblStorage), tr(Str::SdMissing));
  }
  std::string hw;
  hw += deps_.boot.rtcReady ? "RTC OK" : "RTC FAIL";
  hw += deps_.boot.touchReady ? " · TP OK" : " · TP FAIL";
  hw += deps_.boot.buttonsReady ? " · KEY OK" : " · KEY FAIL";
  hw += eego::FrontlightManager::instance().frontlightAvailable() ? " · FL" : "";
  add(tr(Str::LblHardware), hw);
  const esp_app_desc_t* d = esp_app_get_description();
  snprintf(buf, sizeof(buf), "%s %s", d->project_name, d->version);
  add(tr(Str::Version), buf);
  formatUptime(nowMs() / 1000, buf, sizeof(buf));
  add(tr(Str::Uptime), buf);

  section(Str::Sources);
  if (!dashboard_.available()) {
    add(tr(Str::LblDashboard), tr(Str::NoDashboard));
  } else {
    std::string title = dashboard_.title();
    add(tr(Str::LblDashboard), title.empty() ? std::string("-") : title);
    const std::vector<DashboardSourceInfo> sources = dashboard_.sources();
    if (sources.empty()) add("  ", tr(Str::NoSources));
    for (const DashboardSourceInfo& src : sources) {
      std::string value;
      char age[24];
      formatAge(src.last_success_ms > 0 ? static_cast<uint64_t>(src.last_success_ms / 1000) : 0,
                age, sizeof(age));
      if (!src.enabled) {
        value = tr(Str::SourceDisabled);
      } else if (src.consecutive_failures > 0 || src.offline) {
        snprintf(buf, sizeof(buf), "%s ×%u", tr(Str::Fail),
                 static_cast<unsigned>(src.consecutive_failures));
        value = buf;
        if (src.last_http_status > 0) value += " · HTTP " + std::to_string(src.last_http_status);
        if (!src.error.empty()) value += " · " + src.error;
        value += std::string(" · ") + tr(Str::LastOk) + " " + age;
      } else {
        value = src.stale && src.has_data ? tr(Str::SourceStale) : tr(Str::SourceOk);
        value += std::string(" · ") + tr(Str::LastOk) + " " + age;
      }
      std::string label = src.id;
      sanitizeClamped(label, 14);
      add(label, value);
    }
  }

  section(Str::SecAlerts);
  size_t active = 0, acked = 0, muted = 0;
  if (snapshot_ != nullptr) {
    for (const auto& r : snapshot_->records) {
      if (r.muted) {
        muted++;
      } else if (r.state == alert_core::AlertState::Active) {
        active++;
      } else if (r.state == alert_core::AlertState::Acked) {
        acked++;
      }
    }
  }
  snprintf(buf, sizeof(buf), tr(Str::AlertStoreFmt), static_cast<unsigned>(active),
           static_cast<unsigned>(acked), static_cast<unsigned>(muted));
  add("  ", buf);

  big.resize(rows.size(), false);
  std::vector<size_t> starts{0};
  size_t weight = 0;
  for (size_t i = 0; i < rows.size(); ++i) {
    const size_t w = big[i] ? 2 : 1;
    const bool heading = rows[i].second.empty();
    const size_t need = heading && i + 1 < rows.size() ? w + (big[i + 1] ? 2 : 1) : w;
    if (weight > 0 && weight + need > geom::kStatusRowsPerPage) {
      starts.push_back(i);
      weight = 0;
    }
    weight += w;
  }
  statusRows_ = starts.size() * geom::kStatusRowsPerPage;
  const size_t pages = statusPageCount();
  if (statusPage_ >= pages) statusPage_ = pages - 1;
  const size_t begin = starts[statusPage_];
  const size_t end = statusPage_ + 1 < starts.size() ? starts[statusPage_ + 1] : rows.size();
  for (size_t i = begin; i < end; ++i) {
    m.rows.push_back(std::move(rows[i]));
    m.big.push_back(big[i]);
  }
  if (pages > 1) {
    snprintf(m.pageText, sizeof(m.pageText), "%u/%u", static_cast<unsigned>(statusPage_ + 1),
             static_cast<unsigned>(pages));
  }
}

void PanelApp::buildHelp(KvModel& m) {
  auto add = [&m](Str label, Str value) {
    m.rows.emplace_back(tr(label), tr(value));
  };
  add(Str::HelpUpDownShort, Str::HelpScreen);
  add(Str::HelpUpDownLong, Str::HelpFocus);
  add(Str::HelpPower, Str::HelpActivateBack);
  add(Str::HelpFrontHome, Str::HelpHome);
  add(Str::HelpFrontGestures, Str::HelpSettingsStatus);
  add(Str::HelpSwipe, Str::HelpPage);
  add(Str::HelpTapAlert, Str::HelpOpenDetail);
  add(Str::HelpDanger, Str::HelpFreshPress);
  add(Str::HelpUpDownChord, Str::HelpKeepAwake);
  add(Str::HelpCustomBindings, Str::HelpWebBindings);
}

void PanelApp::buildS4(S4Model& m) {
  const std::vector<SettingId> ids = visibleSettings();
  if (ids.empty()) return;
  if (focus_ >= ids.size()) focus_ = ids.size() - 1;
  m.pageCount = (ids.size() + kSettingsRowsPerPage - 1) / kSettingsRowsPerPage;
  m.page = settingsPage();
  m.selected = focus_ % kSettingsRowsPerPage;
  const size_t begin = m.page * kSettingsRowsPerPage;
  const size_t end = std::min(begin + kSettingsRowsPerPage, ids.size());

  char buf[96];
  const eego::ble::Status ble_runtime = eego::ble::BleManager::instance().status();
  for (size_t i = begin; i < end; i++) {
    S4RowModel row;
    std::string value;
    Str label = Str::None;
    std::string customLabel;
    bool isAction = false;
    switch (ids[i]) {
      case SettingId::KeepAwake: {
        snprintf(buf, sizeof(buf), tr(Str::SetKeepAwakeFmt),
                 static_cast<unsigned>(settings_.keep_awake_min));
        customLabel = buf;
        isAction = true;
        const int64_t now = nowMs();
        const uint32_t left = admin::keep_awake::remainingS(now, keepAwakeUntil(now));
        if (left == 0) {
          value = tr(Str::Off);
        } else {
          snprintf(buf, sizeof(buf), tr(Str::StateKeepAwakeFmt),
                   static_cast<unsigned>(admin::keep_awake::displayMinutes(left)));
          value = buf;
          const size_t dot = value.find(" · ");
          if (dot != std::string::npos) value = value.substr(dot + strlen(" · "));
        }
        break;
      }
      case SettingId::Network: {
        const eego::net::NetStatusSnapshot& status = netStatus();
        label = Str::SetNetwork;
        value = status.wifi.ip.empty() ? tr(Str::WifiDisconnected) : status.wifi.ip;
        break;
      }
      case SettingId::RecoveryAp:
        label = Str::SetRecoveryAp;
        isAction = true;
        if (admin::ProvisionPortal::instance().apActive()) {
          value = tr(Str::On);
        } else {
          value = tr(Str::Off);
        }
        break;
      case SettingId::WifiReset:
        label = Str::SetWifiReset;
        isAction = true;
        break;
      case SettingId::Bluetooth:
        label = Str::SetBluetooth;
        if (!ble_runtime.compiled) {
          value = tr(Str::Unavailable);
        } else if (!settings_.ble_admin_enabled) {
          value = tr(Str::Off);
        } else if (ble_runtime.link == eego::ble::LinkState::Authenticated) {
          value = tr(Str::BleStateLinked);
        } else if (ble_runtime.link == eego::ble::LinkState::Pairing) {
          value = tr(Str::BleStatePairing);
        } else if (ble_runtime.advertising) {
          value = tr(Str::BleStateAdvertising);
        } else {
          value = tr(Str::BleStateIdle);
        }
        break;
      case SettingId::Language:
        label = Str::SetLanguage;
        value = settings_.language == "zh" ? tr(Str::LangZh) : tr(Str::LangEn);
        break;
      case SettingId::Timezone: {
        label = Str::SetTimezone;
        const int offset = settings_.timezone_offset_min;
        snprintf(buf, sizeof(buf), "UTC%c%02d:%02d", offset < 0 ? '-' : '+', abs(offset) / 60,
                 abs(offset) % 60);
        value = buf;
        break;
      }
      case SettingId::Frontlight:
        label = Str::SetFrontlight;
        value = settings_.frontlight_enabled ? tr(Str::On) : tr(Str::Off);
        break;
      case SettingId::Brightness:
        label = Str::SetBrightness;
        snprintf(buf, sizeof(buf), "%u%%",
                 static_cast<unsigned>(settings_.frontlight_brightness));
        value = buf;
        break;
      case SettingId::Warmth:
        label = Str::SetWarmth;
        snprintf(buf, sizeof(buf), "%u%%", static_cast<unsigned>(settings_.frontlight_warmth));
        value = buf;
        break;
      case SettingId::DataRefresh:
        label = Str::SetDataRefresh;
        if (settings_.data_refresh_s == 0) {
          value = tr(Str::SourceSchedules);
        } else {
          formatEvery(settings_.data_refresh_s, buf, sizeof(buf));
          value = buf;
        }
        break;
      case SettingId::PollNow:
        label = Str::SetPollNow;
        isAction = true;
        break;
      case SettingId::EpdFull:
        label = Str::SetEpdFull;
        if (settings_.epd_full_refresh_s == 0) {
          value = tr(Str::ManualOnly);
        } else {
          formatEvery(settings_.epd_full_refresh_s, buf, sizeof(buf));
          value = buf;
        }
        break;
      case SettingId::EpdNow:
        label = Str::SetEpdNow;
        isAction = true;
        break;
      case SettingId::IdleAfter:
        label = Str::SetIdleAfter;
        if (settings_.idle_after_min == 0) {
          value = tr(Str::Off);
        } else {
          snprintf(buf, sizeof(buf), tr(Str::IdleAfterFmt),
                   static_cast<unsigned>(settings_.idle_after_min));
          value = buf;
        }
        break;
      case SettingId::IdleRefresh:
        label = Str::SetIdleRefresh;
        formatEvery(settings_.idle_refresh_min * 60, buf, sizeof(buf));
        value = buf;
        break;
      case SettingId::ScreenRotate:
        label = Str::SetScreenRotate;
        if (settings_.screen_rotate_min == 0) {
          value = tr(Str::Off);
        } else {
          formatEvery(settings_.screen_rotate_min * 60, buf, sizeof(buf));
          value = buf;
        }
        break;
      case SettingId::PowerMode:
        label = Str::SetPowerMode;
        value = tr(settings_.power_mode == PowerMode::Mains     ? Str::PowerAlwaysExternal
                   : settings_.power_mode == PowerMode::Battery ? Str::PowerAlwaysBattery
                                                                : Str::ModeAuto);
        break;
      case SettingId::ChargeSignal: {
        using eego::settings::ChargeSignal;
        label = Str::SetChargeSignal;
        const bool high = eego::PowerManager::instance().chargeActiveHigh();
        if (settings_.charge_signal == ChargeSignal::Auto) {
          snprintf(buf, sizeof(buf), "%s · %s", tr(Str::ModeAuto),
                   tr(high ? Str::ChargeActiveHigh : Str::ChargeActiveLow));
          value = buf;
        } else {
          value = tr(high ? Str::ChargeActiveHigh : Str::ChargeActiveLow);
        }
        break;
      }
      case SettingId::BatteryRefresh:
        label = Str::SetBatteryRefresh;
        formatEvery(settings_.battery_refresh_min * 60, buf, sizeof(buf));
        value = buf;
        break;
      case SettingId::BatteryMaxStale:
        label = Str::SetBatteryMaxStale;
        formatEvery(settings_.battery_max_stale_min * 60, buf, sizeof(buf));
        value = buf;
        break;
      case SettingId::BatteryInteractive:
        label = Str::SetBatteryInteractive;
        snprintf(buf, sizeof(buf), tr(Str::TimeoutSecFmt),
                 static_cast<unsigned>(settings_.battery_interactive_timeout_s));
        value = buf;
        break;
      case SettingId::SleepPlan:
        label = Str::SetSleepPlan;
        if (settings_.sleep_start_min < 0) {
          value = tr(Str::NotScheduled);
        } else {
          snprintf(buf, sizeof(buf), "%02d:%02d-%02d:%02d", settings_.sleep_start_min / 60,
                   settings_.sleep_start_min % 60, settings_.sleep_end_min / 60,
                   settings_.sleep_end_min % 60);
          value = buf;
        }
        break;
      case SettingId::NextWake: {
        label = Str::SetNextWake;
        const uint64_t wake = plannedWakeAt();
        if (wake == 0) {
          value = tr(Str::NotScheduled);
        } else {
          formatTs(wake, buf, sizeof(buf));
          value = buf;
        }
        break;
      }
      case SettingId::LastWake: {
        label = Str::SetLastWake;
        value = tr(wakeSourceText(wakeSource_));
        break;
      }
      case SettingId::Storage:
        label = Str::SetStorage;
        value = eego::SdCard::instance().ready() ? tr(Str::SdReady) : tr(Str::SdMissing);
        break;
      case SettingId::Diagnostics:
        label = Str::SetDiagnostics;
        value = settings_.diagnostics_enabled ? tr(Str::On) : tr(Str::Off);
        break;
      case SettingId::RunDiagnostics:
        label = Str::SetRunDiagnostics;
        value = settings_.diagnostics_enabled ? "" : tr(Str::Unavailable);
        isAction = true;
        break;
      case SettingId::Security:
        label = Str::SetSecurity;
        value = admin::AuthManager::instance().hasPassword() ? tr(Str::SecuritySet)
                                                             : tr(Str::SecurityMissing);
        break;
      case SettingId::Mutes:
        label = Str::SetMutes;
        snprintf(buf, sizeof(buf), tr(Str::MutesFmt), static_cast<unsigned>(muteRuleCount_));
        value = buf;
        isAction = true;
        break;
      case SettingId::FactoryReset:
        label = Str::SetFactoryReset;
        isAction = true;
        break;
      case SettingId::Version:
        label = Str::Version;
        value = esp_app_get_description()->version;
        break;
      case SettingId::InputHelp:
        label = Str::SetInputHelp;
        break;
      case SettingId::InputReset:
        label = Str::SetInputReset;
        isAction = true;
        break;
      case SettingId::TouchCal:
        label = Str::SetTouchCal;
        value = deps_.touch == nullptr ? tr(Str::Unavailable)
                : deps_.touch->calibrationCustom() ? tr(Str::TouchCalibrated)
                                                   : tr(Str::TouchFactory);
        isAction = true;
        break;
      case SettingId::TouchMarks:
        label = Str::SetTouchMarks;
        value = touchMarks_ ? tr(Str::On) : tr(Str::Off);
        break;
      case SettingId::BleObserver:
        label = Str::SetBleObserver;
        if (!ble_runtime.compiled) {
          value = tr(Str::Unavailable);
        } else if (ble_runtime.scanning) {
          snprintf(buf, sizeof(buf), "%s %us", tr(Str::On),
                   static_cast<unsigned>(ble_runtime.observer_remaining_s));
          value = buf;
        } else {
          value = settings_.ble_observer_enabled ? tr(Str::On) : tr(Str::Off);
        }
        break;
      case SettingId::BleUnbind:
        label = Str::SetBleUnbind;
        if (!ble_runtime.compiled) value = tr(Str::Unavailable);
        else isAction = true;
        break;
      case SettingId::Count:
        break;
    }
    row.label = customLabel.empty() ? tr(label) : customLabel;
    row.value = std::move(value);
    row.isAction = isAction;
    m.rows.push_back(std::move(row));
  }
}

}  // namespace panel
