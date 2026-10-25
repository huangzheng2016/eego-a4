// SPDX-License-Identifier: GPL-3.0-or-later
// 监控面板设备应用状态机实现（契约见 panel_app.h）。
#include "panel_app.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "admin/admin.h"
#include "admin/provision_portal.h"
#include "dashboard_ui/preview_render.h"
#include "alert_core/types.h"
#include "ble/ble_manager.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "net_stack/net_stack.h"
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
constexpr uint32_t kPollWaitMs = 45000;
constexpr uint32_t kRenderThrottleMs = 250;
constexpr int64_t kClockCheckMs = 1000;          // 状态栏分钟变化检查周期（仅分钟变化才重绘）
constexpr int64_t kBleProvRenderMs = 10000;      // BLE 配网倒计时最快 10s 重绘一次
constexpr int64_t kDoublePressSyncMs = 1000;     // 绑定修订号复查周期
constexpr uint32_t kNetIdleProbeMs = 100;        // 单次 waitIdle 探测超时（入队 + 等待各 ≤100ms）
constexpr int64_t kNetIdleFirstIntervalMs = 1000;
constexpr int64_t kNetIdleMaxIntervalMs = 5000;
constexpr int64_t kPollIdleBudgetMs = 40000;     // 原 waitIdle(30000) + 重试 waitIdle(10000)
constexpr int64_t kSleepIdleBudgetMs = 15000;    // 原 waitIdle(10000) + 重试 waitIdle(5000)
constexpr size_t kSettingsRowsPerPage = static_cast<size_t>(geom::kSettingsRowsPerPage);
constexpr int64_t kDetailCheckMs = 1000;        // 告警详情内容变化检查周期
constexpr int64_t kOnboardingCheckMs = 2000;    // 引导页地址/WiFi 变化检查周期
constexpr int64_t kStatusRefreshMs = 30000;     // 状态页周期重绘（年龄/运行时长）

constexpr uint32_t kDashboardPollWaitMs = 130000;

void localTm(const eego::settings::Settings& settings, uint64_t unix, struct tm& out) {
  const time_t local = static_cast<time_t>(static_cast<int64_t>(unix) +
                                           static_cast<int64_t>(settings.timezone_offset_min) * 60);
  gmtime_r(&local, &out);
}

// AdminEvent 小信箱元素（httpd/sys_evt → app_task）
struct AdminMail {
  int type;
  int reason;
  uint32_t remainingS;
};

// AdminEventType 数值（admin/admin_types.h；以 int 传递避免头依赖扩散）
constexpr int kEvPortalOpened = 0;
constexpr int kEvPortalClosed = 1;
constexpr int kEvApFallbackOpened = 2;
constexpr int kEvApFallbackClosed = 3;
constexpr int kEvProvisionSucceeded = 4;

}  // namespace

int64_t PanelApp::nowMs() const { return esp_timer_get_time() / 1000; }

// ── 生命周期 ────────────────────────────────────────────────────────────────

void PanelApp::begin(const Deps& deps) {
  deps_ = deps;
  settings_ = eego::settings::DeviceSettings::instance().snapshot().values;
  wakeCause_ = static_cast<int>(esp_sleep_get_wakeup_cause());
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

  mains_ = computeMains();
  NetStack::instance().wifi().setPowerSave(mains_);
  if (!mains_) {
    battPhase_ = BattPhase::Polling;
    cycleStartMs_ = lastBatMs_;
    pollDeadlineMs_ = cycleStartMs_ + kPollWaitMs;
  }

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
  dashboard_.setMains(mains_);
  syncDoublePress(true);

  ESP_LOGI(TAG, "panel begin: mains=%d lang=%s bat=%umV/%u%%", mains_ ? 1 : 0,
           settings_.language.c_str(), batCache_.millivolts, batCache_.percent);
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
    // 前光不可用（未探测到且无 present 覆盖）时跳过：硬件覆盖变化由 app_main
    // 的设置订阅统一处理（setOverride + capabilities）。
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
        // 前光行显隐变化：focus_ 是行下标，按 SettingId 重映射，保持焦点在同一
        // 设置上（若该行被隐藏，落到其后第一个仍可见的行）。
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
      markDirty(true);  // 前光行显隐变化 → 设置页整页重排
    }
    frontlightRowsVisible_ = visibleNow;
  }
  if ((changed & eego::settings::kChangedDataRefresh) != 0) {
    // 全局刷新周期覆盖 dashboard 面板（面板上单独设定的周期优先）。
    dashboard_.setGlobalRefresh(settings_.data_refresh_s * 1000);
  }
  if ((changed & eego::settings::kChangedEpdRefresh) != 0) {
    lastFullRenderMs_ = nowMs();
  }
  if ((changed & eego::settings::kChangedSleep) != 0) {
    lastScheduleCheckMs_ = nowMs() - 1001;
    if (battPhase_ == BattPhase::Serving && settings_.idle_sleep_s > 0) {
      awakeDeadlineMs_ = nowMs() + static_cast<int64_t>(settings_.idle_sleep_s) * 1000;
    }
  }
  if ((changed & eego::settings::kChangedBle) != 0) {
    eego::ble::BleManager::instance().configure(settings_);
  }
  if ((changed & (eego::settings::kChangedPower | eego::settings::kChangedBle)) != 0) {
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
}

uint64_t PanelApp::nextWakeAt(int64_t now_ms) const {
  if (admin::ProvisionPortal::instance().apActive() ||
      eego::ble::BleManager::instance().sleepBlocked()) {
    return 0;
  }
  const uint64_t now_unix = TimeSource::now();
  uint64_t wake_at = 0;
  uint32_t remaining = 0;
  if (eego::settings::scheduledSleep(settings_, now_unix, wake_at, remaining)) return wake_at;
  if (mains_ || settings_.idle_sleep_s == 0) return 0;
  const int64_t cycle_end = cycleStartMs_ + static_cast<int64_t>(settings_.battery_poll_s) * 1000;
  int64_t left = cycle_end > now_ms ? cycle_end - now_ms : 0;
  if (left < 10000) left = 10000;
  return now_unix + static_cast<uint64_t>(left / 1000);
}

// ── 跨任务通知（短平快）─────────────────────────────────────────────────────

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
  netStatusCacheMs_ = 0;  // WiFi 跃迁 → 立即刷新地址/信号缓存
  if (page_ != Page::Dashboard) markDirty();
  // WiFi 全账号失败 → AP 兜底（auto_ap_fallback 判定在 app_task，M13 F2）
  if ((bits & kNetBitWifi) != 0 && wifiState_.load() == 4 /*OfflineMode*/ &&
      !fallbackArmed_) {
    fallbackArmed_ = true;
    if (admin::ConfigStore::instance().admin().auto_ap_fallback &&
        !admin::ProvisionPortal::instance().apActive()) {
      ESP_LOGW(TAG, "wifi offline: opening AP fallback");
      admin::ProvisionPortal::instance().openApFallback();
    }
  }
  if (wifiState_.load() == 2 /*Connected*/) fallbackArmed_ = false;
}

void PanelApp::drainAdminMailbox() {
  if (adminQueue_ == nullptr) return;
  AdminMail mail;
  while (xQueueReceive(static_cast<QueueHandle_t>(adminQueue_), &mail, 0) == pdTRUE) {
    switch (mail.type) {
      case kEvPortalOpened:
        enterProvision();
        break;
      case kEvPortalClosed:
        if (page_ == Page::Provision) leaveProvision();
        break;
      case kEvApFallbackOpened:
        netStatusCacheMs_ = 0;
        showBanner(tr(Str::ApFallback), 5000);
        break;
      case kEvApFallbackClosed:
        netStatusCacheMs_ = 0;
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

// ── 主循环 ──────────────────────────────────────────────────────────────────

void PanelApp::service() {
  const int64_t now = nowMs();

  drainAdminMailbox();
  drainNetBits();

  // 外部告警快照（无锁 acquire）：只有告警详情/状态页直接展示 store 内容；
  // dashboard 告警面板由 DashboardRuntime::service 自行跟踪快照代际。
  if (deps_.publisher != nullptr) {
    auto snap = deps_.publisher->acquire();
    if (snap && snap->seq != snapshotSeq_) {
      snapshot_ = snap;
      snapshotSeq_ = snap->seq;
      if (page_ == Page::AlertDetail || page_ == Page::Status) markDirty();
    }
  }

  refreshBattery(now);
  if (now - lastChargeMs_ > 30000) {
    charging_ = eego::PowerManager::instance().isCharging();
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
  if (wantMains != mains_) onModeChange(wantMains);
  dashboard_.service(mains_);
  // web 端应用/删除 dashboard 后 runtime 自行重载；主页随可用性在
  // Dashboard ↔ Onboarding 之间切换，依赖 dashboard 的告警详情一并退出。
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
  eego::ble::BleManager::instance().service(mains_);
  const eego::ble::Status ble_status = eego::ble::BleManager::instance().status();
  if (ble_status.role != eego::ble::Role::Provisioning) {
    clearBlePop();
    if (bleProvisionWasActive_) {
      bleProvisionWasActive_ = false;
      showBanner(ble_status.provisioning == eego::ble::ProvisionState::Succeeded
                     ? tr(Str::BleProvSuccess)
                     : tr(Str::BleProvFailed),
                 5000);
    }
  }

  if (now - lastScheduleCheckMs_ > 1000) {
    lastScheduleCheckMs_ = now;
    // BLE 配网倒计时（设置页 PoP 行）：最快 10s 重绘一次，避免每秒刷屏
    if (ble_status.role == eego::ble::Role::Provisioning && page_ == Page::Settings &&
        now - lastBleProvRenderMs_ >= kBleProvRenderMs) {
      lastBleProvRenderMs_ = now;
      markDirty();
    }
    if (!sleepPending_ && !admin::ProvisionPortal::instance().apActive() &&
        !eego::ble::BleManager::instance().sleepBlocked()) {
      uint64_t wake_at = 0;
      uint32_t remaining = 0;
      if (eego::settings::scheduledSleep(settings_, nowUnix(), wake_at, remaining)) {
        enterSleep(now, static_cast<int64_t>(remaining) * 1000);
      }
    }
  }
  batteryFsm(now);
  serviceSleep(now);

  if (settings_.epd_full_refresh_s > 0 &&
      now - lastFullRenderMs_ >= static_cast<int64_t>(settings_.epd_full_refresh_s) * 1000) {
    if (page_ == Page::Dashboard) {
      dashboard_.invalidateAll();
      forceFull_ = true;
    } else {
      markDirty(true);
    }
  }

  if (page_ == Page::Dashboard) {
    if (dashboard_.needsRender()) {
      dashboard_.render();
      noteInputResume();
      if (forceFull_) lastFullRenderMs_ = now;
      dirty_ = false;
      forceFull_ = false;
      lastRenderMs_ = now;
    }
    return;
  }

  // 状态栏时钟：仅当显示的分钟（或时间可用性）变化时重绘
  if (now - lastClockCheckMs_ >= kClockCheckMs) {
    lastClockCheckMs_ = now;
    const int64_t key =
        TimeSource::valid() ? static_cast<int64_t>(TimeSource::now() / 60) : -1;
    if (key != renderedClockKey_) markDirty();
  }

  if (dirty_ && now - lastRenderMs_ >= kRenderThrottleMs) {
    render(now);
  }
}

// ── 输入 ────────────────────────────────────────────────────────────────────

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
  // 渲染等阻塞期间入队的事件：以 app_task 恢复时刻起算年龄。
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
    // 同一按键手势的后续事件沿用其 Press 的判定（整段保留或整段丢弃）。
    stale = buttonGestureStale_[index] || ev.timestamp_us <= 0 || now_us < ev.timestamp_us;
  }
  if (stale) return;
  lastInputMs_ = now_us / 1000;
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
        // 实体 Power 持续按住是最强确认：触控/焦点布防的控件一律可由 Power
        // 确认，与当前焦点无关（触控点按不移动实体焦点）。
        control = wanted;
      } else {
        control = focusedControl();
      }
      if (confirmState_.beginHold(source, control, ev.timestamp_us, now_us)) return;
      clearConfirm();
    }
    return;
  }
  if (ev.action == eego::ButtonAction::Release) {
    if (confirm_ != ConfirmKind::None && confirmState_.holding() &&
        confirmState_.source() == source) {
      suppressButtonShort_[index] = true;
      clearConfirm();
    }
    return;
  }
  if (ev.action == eego::ButtonAction::LongPress) {
    if (confirm_ != ConfirmKind::None) {
      suppressButtonShort_[index] = true;
      if (confirmState_.completeHold(source, ev.timestamp_us, now_us)) {
        executeArmedConfirm();
      } else {
        clearConfirm();
      }
      return;
    }
    if (ev.id == eego::ButtonId::Power) {
      activeInputControl_ = focusedControl();
      back();
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
    if (confirm_ != ConfirmKind::None) clearConfirm();
    if (ev.id == eego::ButtonId::Power) {
      activeInputControl_ = focusedControl();
      activateFocused();
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
    // 同一手势的后续事件（Move/Release/Tap/Hold/Swipe…）沿用起始 Press 的判定。
    stale = gestureStale || ev.timestamp_us <= 0 || now_us < ev.timestamp_us;
  }
  if (stale) return;
  lastInputMs_ = now_us / 1000;
  const int16_t x = static_cast<int16_t>(ev.x);
  const int16_t y = static_cast<int16_t>(ev.y);
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
        if (confirmState_.beginHold(ConfirmSource::Touch, touchControl(x, y),
                                    ev.timestamp_us, now_us)) {
          return;
        }
        clearConfirm();
      }
      break;
    case eego::TouchEventType::Release:
      if (confirm_ != ConfirmKind::None && confirmState_.holding() &&
          confirmState_.source() == ConfirmSource::Touch) {
        suppressTouchTap_ = true;
        clearConfirm();
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
        if (confirmState_.completeHold(ConfirmSource::Touch, ev.timestamp_us, now_us)) {
          executeArmedConfirm();
        } else {
          clearConfirm();
        }
      } else {
        onTouchHold(x, y);
      }
      break;
    case eego::TouchEventType::Tap:
      if (suppressTouchTap_) {
        suppressTouchTap_ = false;
        return;
      }
      if (confirm_ != ConfirmKind::None) clearConfirm();
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
        if (confirmState_.beginHold(ConfirmSource::Front, control, ev.timestamp_us, now_us)) {
          return;
        }
        clearConfirm();
      }
      break;
    case eego::TouchEventType::FrontRelease:
      if (confirm_ != ConfirmKind::None && confirmState_.holding() &&
          confirmState_.source() == ConfirmSource::Front) {
        suppressFrontShort_ = true;
        clearConfirm();
      }
      break;
    case eego::TouchEventType::FrontShort:
      if (suppressFrontShort_) {
        suppressFrontShort_ = false;
        return;
      }
      if (confirm_ != ConfirmKind::None) clearConfirm();
      activeInputControl_ = "front.home";
      goHome();
      break;
    case eego::TouchEventType::FrontLong:
      if (confirm_ != ConfirmKind::None) {
        if (confirmState_.completeHold(ConfirmSource::Front, ev.timestamp_us, now_us)) {
          executeArmedConfirm();
        } else {
          clearConfirm();
        }
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

// ── 导航 ────────────────────────────────────────────────────────────────────

namespace {

size_t wrappedIndex(size_t current, int delta, size_t count) {
  if (count == 0) return 0;
  int64_t next = (static_cast<int64_t>(current) + delta) % static_cast<int64_t>(count);
  if (next < 0) next += static_cast<int64_t>(count);
  return static_cast<size_t>(next);
}

// 有序取值表单步：dir>0 下一档、dir<0 上一档；wrap=false 时两端钳制。
// current 不在表中时取方向上最近的一档。
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

constexpr uint8_t kBrightnessSteps[] = {10, 25, 40, 60, 80, 100};  // 含默认 60
constexpr uint8_t kWarmthSteps[] = {0, 25, 50, 75, 100};
constexpr uint32_t kDataRefreshSteps[] = {0, 60, 300, 900, 1800, 3600};
constexpr uint32_t kEpdFullSteps[] = {0, 3600, 21600, 86400, 604800};
constexpr uint32_t kBatteryPollSteps[] = {300, 900, 1800, 3600, 10800, 21600};
constexpr uint32_t kIdleSleepSteps[] = {0, 30, 60, 90, 180, 300, 600};
constexpr int kTimezoneMinMin = -720;
constexpr int kTimezoneMaxMin = 840;

struct SleepPlanPreset {
  int start;
  int end;
};
constexpr SleepPlanPreset kSleepPlans[] = {
    {-1, -1}, {22 * 60, 6 * 60}, {23 * 60, 7 * 60}, {0, 6 * 60}, {12 * 60 + 30, 13 * 60 + 30}};
constexpr size_t kSleepPlanCount = sizeof(kSleepPlans) / sizeof(kSleepPlans[0]);

// "每 N 秒/分钟/小时/天"：整除时取最大单位。
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

// Page 维度：dashboard 上/下一屏；设置行与告警列表的上/下一项；状态子页。
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
      markDirty(settingsPage() != oldPage);  // 同页焦点移动走 Fast；跨子页整页重绘
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

void PanelApp::gotoPage(Page p) {
  if (page_ == p || (p == Page::Dashboard && !dashboard_.available())) return;
  page_ = p;
  ++pageEpoch_;
  navigation_.cancel();
  // 详情页默认聚焦「确认」软键；其余页从第一个软键/行开始。
  focus_ = 0;
  statusPage_ = 0;
  provisionSucceeded_ = false;
  clearConfirm();
  if (p == Page::Settings) refreshMuteRuleCount();
  if (p == Page::Dashboard) dashboard_.invalidateAll();
  if (p == Page::Onboarding) onboardingKey_.clear();
  syncDoublePress(false);
  markDirty(true);  // 切页 → 下一帧 Full
}

void PanelApp::back() {
  switch (page_) {
    case Page::AlertDetail:
      gotoPage(home());
      break;
    case Page::Help:
      // 帮助页的来源页可能已不可进入（dashboard 期间被卸载/失效）：gotoPage 会
      // 静默拒绝，Back 就卡在帮助页——此时回 home()。
      gotoPage(helpReturn_ == Page::Help || helpReturn_ == Page::Provision ||
                       (helpReturn_ == Page::Dashboard && !dashboard_.available())
                   ? home()
                   : helpReturn_);
      break;
    case Page::Status:
    case Page::Settings:
      gotoPage(home());
      break;
    case Page::Dashboard:
      // 先退 dashboard 内部返回栈（跳转动作），否则回到默认页第一屏。
      if (!dashboard_.back() && !dashboard_.atHome()) dashboard_.home();
      break;
    default:
      break;
  }
}

// Focus 维度：dashboard 面板焦点；设置子页跳转；其余页软键焦点。
void PanelApp::moveFocus(int delta) {
  switch (page_) {
    case Page::Dashboard:
      dashboard_.moveFocus(delta, false);
      break;
    case Page::Settings: {
      // 设置页不循环：与页码条两端置灰的箭头一致。
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
      // [状态] [设置]
      if (idx == 0) gotoPage(Page::Status);
      else if (idx == 1) gotoPage(Page::Settings);
      break;
    case Page::AlertDetail: {
      // [确认] [静音] [删除] [趋势?] [返回]
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
      else if (idx == 1) back();
      break;
    case Page::Settings:
      // [← 上一页] [返回] [下一页 →]
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

// ── 设置行 ──────────────────────────────────────────────────────────────────

std::vector<SettingId> PanelApp::visibleSettings() const {
  // 前光不可用（未探测到且无 present 覆盖）→ 隐藏三行前光设置。
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
    case SettingId::PowerMode:
    case SettingId::BatteryPoll:
    case SettingId::IdleSleep:
    case SettingId::SleepPlan:
    case SettingId::Diagnostics:
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
    case SettingId::PowerMode: {
      constexpr PowerMode kModes[] = {PowerMode::Auto, PowerMode::Mains, PowerMode::Battery};
      next.power_mode = stepChoice(kModes, settings_.power_mode, step, true);
      break;
    }
    case SettingId::BatteryPoll:
      next.battery_poll_s = stepChoice(kBatteryPollSteps, settings_.battery_poll_s, step, wrap);
      break;
    case SettingId::IdleSleep:
      next.idle_sleep_s = stepChoice(kIdleSleepSteps, settings_.idle_sleep_s, step, wrap);
      break;
    case SettingId::SleepPlan: {
      size_t idx = kSleepPlanCount;  // 自定义计划（web 设置）视为不在预设表中
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

// ── 触控 ────────────────────────────────────────────────────────────────────

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
    if (geom::kSettingsPrevArrow.contains(x, y)) {
      hit.kind = TouchHit::Kind::PagePrev;
      return hit;
    }
    if (geom::kSettingsNextArrow.contains(x, y)) {
      hit.kind = TouchHit::Kind::PageNext;
      return hit;
    }
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
    case TouchHit::Kind::PagePrev:
      return prefix + ":pager:prev";
    case TouchHit::Kind::PageNext:
      return prefix + ":pager:next";
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
    case TouchHit::Kind::PagePrev:
      moveFocus(-1);  // 设置页页码条：上一子页
      return;
    case TouchHit::Kind::PageNext:
      moveFocus(1);
      return;
    case TouchHit::Kind::Row: {
      // 一次点按 = 聚焦 + 激活（危险动作仍经 armConfirm 布防）；点值列左/右半
      // 对取值行做 -1/+1 调整。
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
  // 设置页长按值列：时区按 6 小时步进，其余取值行同点按（±1 档）。
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
  // 左滑/上滑 = 下一屏（项）；右滑/下滑 = 上一屏（项）。子页面在首屏（首项）
  // 右滑 = 返回；dashboard 是主页，不返回。
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
        break;  // 已在边界：不循环
      }
      requestNavigation(NavigationKind::Focus, delta, now_us);  // 设置子页
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
  // 双击检测会把 Up/Down 短按延迟 kInputDoublePressUs：只在当前页确有
  // up.double / down.double 有效绑定时开启。
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
    if (page_ == Page::Provision) return false;  // 配网页不分发绑定
    eego::admin::InputBinding binding;
    return eego::admin::resolveInputBinding(set.bindings, context, trigger, binding) &&
           binding.action.kind != eego::admin::InputActionKind::None;
  };
  eego::ButtonManager& buttons = eego::ButtonManager::instance();
  buttons.setDoublePressEnabled(eego::ButtonId::Up, bound(eego::admin::InputTrigger::UpDouble));
  buttons.setDoublePressEnabled(eego::ButtonId::Down,
                                bound(eego::admin::InputTrigger::DownDouble));
}

// ── 动作 ────────────────────────────────────────────────────────────────────

// 告警详情「确认」：外部告警经 net_task 落 store（Active → Acked）并在
// dashboard 视图中压制本代际；规则派生事件只在 dashboard 视图压制。
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
    // dashboard 未持有该事件（理论上不会发生）：直接确认 store 记录。
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
  // store 级静音（M21）：指纹规则经 net_task 落入 AlertStore（重估 muted
  // 标志 + 快照发布），并持久化到 config.json（重启后恢复）。
  // query→append→set 全程在 app_task 上下文，与 admin 配置热生效同任务串行，
  // 无读改写竞争。
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
    rule.expires_at = 0;  // 面板静音语义：不过期（清除走 S4 清静音）
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
  // 静音记录不再进入 dashboard 事件：详情列表下一次刷新时移到相邻条目。
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
  // 取值行：前进一档（循环）；动作/查看行：执行或横幅展示。
  if (settingAdjustable(id)) {
    adjustSettingRow(id, 1, false, true);
    return;
  }
  switch (id) {
    case SettingId::Network: {
      const eego::net::NetStatusSnapshot& status = netStatus();
      if (!status.wifi.ip.empty()) showBannerFmt(5000, "http://%s/", status.wifi.ip.c_str());
      else showBanner(tr(Str::WifiDisconnected), 3000);
      break;
    }
    case SettingId::RecoveryAp: armConfirm(ConfirmKind::NetworkRecovery); break;
    case SettingId::WifiReset: armConfirm(ConfirmKind::WifiReset); break;
    case SettingId::Bluetooth:
      if (eego::ble::BleManager::instance().provisioningActive()) {
        eego::ble::BleManager::instance().cancelProvisioning();
        clearBlePop();
        bleProvisionWasActive_ = false;
        showBanner(tr(Str::BleProvFailed), 3000);
      } else if (!eego::ble::BleManager::instance().status().compiled) {
        showBanner(tr(Str::Unavailable), 3000);
      } else {
        armConfirm(ConfirmKind::BleProvision);
      }
      break;
    case SettingId::PollNow: doPollNow(); break;
    case SettingId::EpdNow:
      markDirty(true);
      showBanner(tr(Str::FullRefreshQueued), 3000);
      break;
    case SettingId::NextWake: {
      const uint64_t wake = nextWakeAt(nowMs());
      if (wake == 0) showBanner(tr(Str::NotScheduled), 3000);
      else {
        char text[32];
        formatTs(wake, text, sizeof(text));
        showBanner(text, 4000);
      }
      break;
    }
    case SettingId::LastWake: {
      Str text = Str::WakeOther;
      if (wakeCause_ == ESP_SLEEP_WAKEUP_TIMER) text = Str::WakeTimer;
      else if (wakeCause_ == ESP_SLEEP_WAKEUP_EXT0) text = Str::WakeButton;
      else if (wakeCause_ == ESP_SLEEP_WAKEUP_UNDEFINED) text = Str::WakeReset;
      showBanner(tr(text), 3000);
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
  esp_err_t err = ESP_OK;
  if (!admin::ProvisionPortal::instance().apActive()) {
    err = admin::ProvisionPortal::instance().openApFallback();
  }
  if (err != ESP_OK &&
      !(err == ESP_ERR_INVALID_STATE && admin::ProvisionPortal::instance().apActive())) {
    showBannerFmt(4000, "%s: %s", tr(Str::ActionFailed), esp_err_to_name(err));
    return;
  }
  awakeDeadlineMs_ = nowMs() + 600000;
  showBanner(tr(Str::RecoveryApStarted), 7000);
  markDirty(true);
}

void PanelApp::clearBlePop() {
  if (blePop_.empty()) return;
  volatile uint8_t* data = reinterpret_cast<volatile uint8_t*>(blePop_.data());
  for (size_t i = 0; i < blePop_.size(); i++) data[i] = 0;
  blePop_.clear();
  markDirty();
}

void PanelApp::doStartBleProvision() {
  if (admin::ProvisionPortal::instance().apActive()) {
    showBanner(tr(Str::Unavailable), 3000);
    return;
  }
  char pop[9];
  std::snprintf(pop, sizeof(pop), "%08lu",
                static_cast<unsigned long>(esp_random() % 100000000UL));
  blePop_ = pop;
  volatile uint8_t* data = reinterpret_cast<volatile uint8_t*>(pop);
  for (size_t i = 0; i < sizeof(pop); i++) data[i] = 0;
  const esp_err_t err = eego::ble::BleManager::instance().startProvisioningFromLocal(blePop_);
  if (err != ESP_OK) {
    clearBlePop();
    showBannerFmt(4000, "%s: %s", tr(Str::ActionFailed), esp_err_to_name(err));
    return;
  }
  bleProvisionWasActive_ = true;
  if (page_ == Page::Settings) {
    const std::vector<SettingId> rows = visibleSettings();
    for (size_t i = 0; i < rows.size(); ++i) {
      if (rows[i] == SettingId::Bluetooth) focus_ = i;
    }
  }
  lastBleProvRenderMs_ = nowMs();
  awakeDeadlineMs_ = nowMs() + 180000;
  showBanner(tr(Str::BleProvStarted), 7000);
  markDirty(true);
}

void PanelApp::doClearBleBonds() {
  bleProvisionWasActive_ = false;
  const esp_err_t err = eego::ble::BleManager::instance().clearBonds();
  if (err == ESP_OK) {
    clearBlePop();
    showBanner(tr(Str::BleUnbindDone), 4000);
  } else {
    showBannerFmt(4000, "%s: %s", tr(Str::ActionFailed), esp_err_to_name(err));
  }
  markDirty(true);
}

void PanelApp::doWifiReset() {
  admin::AdminCommand cmd;
  cmd.type = admin::AdminCommandType::WifiReset;
  admin::AdminResult result;
  admin::defaultCommandHandler(cmd, result, nullptr);
  if (result.ok) showBanner(tr(Str::WifiResetDone), 7000);
  else showBannerFmt(5000, "%s: %s", tr(Str::ActionFailed), result.err.c_str());
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
    showBannerFmt(5000, "%s: %s", tr(Str::ActionFailed), result.err.c_str());
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
  const bool ok = dashboard_.renderPreview(canvas, request.document->dashboard, request.page_id,
                                           request.has_screen ? request.screen : 0, info);
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
    case ConfirmKind::BleProvision: doStartBleProvision(); break;
    case ConfirmKind::BleUnbind: doClearBleBonds(); break;
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
  showBanner(requiresPhysicalConfirm(kind) ? tr(Str::ConfirmPhysicalHold) : tr(Str::ConfirmHold),
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
  showBanner(tr(Str::ConfirmHold), static_cast<uint32_t>(kConfirmWindowUs / 1000));
}

void PanelApp::clearConfirm() {
  if (confirm_ == ConfirmKind::None) return;
  if (confirmState_.holding()) {
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
  if (banner_ == tr(Str::ConfirmHold) || banner_ == tr(Str::ConfirmPhysicalHold)) banner_.clear();
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

// ── 电源模式 ────────────────────────────────────────────────────────────────

bool PanelApp::computeMains() {
  switch (settings_.power_mode) {
    case PowerMode::Mains:
      return true;
    case PowerMode::Battery:
      return false;
    default:
      // 无电池读数（台式/未装电池）视为常供电，避免误判进休眠
      if (batCache_.millivolts == 0) return true;
      return charging_ || batCache_.charging;
  }
}

void PanelApp::onModeChange(bool mains) {
  mains_ = mains;
  dashboard_.setMains(mains);
  ESP_LOGI(TAG, "power mode -> %s", mains ? "mains" : "battery");
  NetStack::instance().wifi().setPowerSave(mains);  // 常供电 modem-sleep
  eego::ble::BleManager::instance().onPowerMode(mains);
  eego::ble::BleManager::instance().setBatteryScanWindow(false);
  pollIdleWait_.active = false;
  if (mains && sleepPending_ && !sleepScheduled_) {
    sleepPending_ = false;  // 常供电不再走空闲休眠
    sleepIdleWait_.active = false;
  }
  if (mains) {
    battPhase_ = BattPhase::Mains;
    batteryPollStarted_ = false;
    NetStack::instance().reconnectNow();
  } else {
    battPhase_ = BattPhase::Polling;
    batteryPollStarted_ = false;
    cycleStartMs_ = nowMs();
    pollDeadlineMs_ = cycleStartMs_ + kPollWaitMs;
    showBanner(tr(Str::Polling), 5000);
    NetStack::instance().reconnectNow();
  }
  markDirty(true);
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
  // net_task 按序处理命令：WaitIdle 被执行即说明此前入队的工作全部完成。
  // 超时的探测命令留在队列中稍后被无害消费；退避间隔把它们限制在个位数。
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

void PanelApp::batteryFsm(int64_t now) {
  if (admin::ProvisionPortal::instance().apActive() ||
      eego::ble::BleManager::instance().provisioningActive()) {
    if (battPhase_ == BattPhase::Serving) awakeDeadlineMs_ = now + 60000;
    return;
  }
  switch (battPhase_) {
    case BattPhase::Mains:
      return;
    case BattPhase::Polling: {
      if (!batteryPollStarted_) {
        if (wifiState_.load() == 2 /*Connected*/ || now > pollDeadlineMs_) {
          ESP_LOGI(TAG, "battery poll cycle: refresh dashboard sources");
          dashboard_.pollBattery();
          batteryPollStarted_ = true;
          pollIdleWait_.active = false;
          pollDeadlineMs_ = now + kDashboardPollWaitMs;
          markDirty(true);
        }
        return;
      }

      if (!pollIdleWait_.active) {
        const bool dashboard_idle = dashboard_.networkIdle();
        if (!dashboard_idle && now <= pollDeadlineMs_) return;
        if (!dashboard_idle) {
          ESP_LOGW(TAG, "battery poll: dashboard still busy, serving cached data");
        }
        // 非阻塞等待 net_task 静默（预算 = 原 30s + 重试 10s），app_task 期间照常
        // 处理输入与渲染。
        startNetIdleWait(pollIdleWait_, now, kPollIdleBudgetMs);
      }
      const int idle = pollNetIdle(pollIdleWait_, now);
      if (idle == 0) return;
      if (idle < 0) {
        ESP_LOGW(TAG, "battery poll: net still not idle, serving with current data");
      }
      battPhase_ = BattPhase::Serving;
      eego::ble::BleManager::instance().setBatteryScanWindow(true);
      awakeDeadlineMs_ = now + static_cast<int64_t>(settings_.idle_sleep_s) * 1000;
      markDirty(true);
      return;
    }
    case BattPhase::Serving: {
      if (settings_.idle_sleep_s == 0) return;
      const int64_t idleMs = static_cast<int64_t>(settings_.idle_sleep_s) * 1000;
      const int64_t hardCapMs = cycleStartMs_ + 600000;
      if (now - lastInputMs_ < 15000 && now < hardCapMs) {
        awakeDeadlineMs_ = now + idleMs;
        if (awakeDeadlineMs_ > hardCapMs) awakeDeadlineMs_ = hardCapMs;
      }
      if (now > awakeDeadlineMs_) enterSleep(now);
      return;
    }
  }
}

void PanelApp::deferSleep(int64_t now) {
  sleepPending_ = false;
  sleepIdleWait_.active = false;
  battPhase_ = mains_ ? BattPhase::Mains : BattPhase::Serving;
  awakeDeadlineMs_ = now + 30000;
  lastScheduleCheckMs_ = now + 30000;
  if (eego::FrontlightManager::instance().frontlightAvailable()) {
    applySettings(eego::settings::kChangedFrontlight);
  }
  markDirty();
}

void PanelApp::enterSleep(int64_t now, int64_t durationMs) {
  if (sleepPending_) return;  // 已在等待网络静默
  ESP_LOGI(TAG, "sleep requested");
  if (!dashboard_.networkIdle()) {
    ESP_LOGW(TAG, "battery: dashboard work pending, sleep deferred");
    deferSleep(now);
    return;
  }
  // 1. 网络静默（非阻塞；预算 = 原 10s + 重试 5s；超时则中止本次休眠回 Serving，
  //    30s 后重试——绝不带在途 HTTP 传输断电 WiFi）
  sleepPending_ = true;
  sleepScheduled_ = durationMs >= 0;
  sleepRequestedMs_ = now;
  sleepDurationMs_ = durationMs;
  startNetIdleWait(sleepIdleWait_, now, kSleepIdleBudgetMs);
  serviceSleep(now);
}

void PanelApp::serviceSleep(int64_t now) {
  if (!sleepPending_) return;
  // 电池交互窗口内（硬上限前）有新输入 → 取消本次空闲休眠，回 Serving 续期。
  if (!sleepScheduled_ && lastInputMs_ > sleepRequestedMs_ &&
      now < cycleStartMs_ + 600000) {
    ESP_LOGI(TAG, "sleep cancelled by input");
    sleepPending_ = false;
    sleepIdleWait_.active = false;
    if (battPhase_ == BattPhase::Serving && settings_.idle_sleep_s > 0) {
      awakeDeadlineMs_ = now + static_cast<int64_t>(settings_.idle_sleep_s) * 1000;
    }
    return;
  }
  if (admin::ProvisionPortal::instance().apActive() ||
      eego::ble::BleManager::instance().provisioningActive()) {
    ESP_LOGW(TAG, "recovery/provisioning became active, sleep deferred");
    deferSleep(now);
    return;
  }
  const int idle = pollNetIdle(sleepIdleWait_, now);
  if (idle == 0) return;
  if (idle < 0) {
    ESP_LOGW(TAG, "net not idle after retries, sleep aborted this round");
    deferSleep(now);
    return;
  }
  if (!dashboard_.networkIdle()) {
    ESP_LOGW(TAG, "battery: dashboard work started while waiting, sleep deferred");
    deferSleep(now);
    return;
  }
  int64_t sleepMs = sleepDurationMs_;
  if (sleepMs < 0) {
    sleepMs = cycleStartMs_ + static_cast<int64_t>(settings_.battery_poll_s) * 1000 - now;
  } else {
    sleepMs -= now - sleepRequestedMs_;  // 计划睡眠：扣除等待静默耗时
  }
  sleepPending_ = false;
  finishSleep(now, sleepMs);
}

void PanelApp::finishSleep(int64_t now, int64_t sleepMs) {
  (void)now;
  if (sleepMs < 10000) sleepMs = 10000;
  if (eego::ble::BleManager::instance().beforeSleep() != ESP_OK) {
    ESP_LOGW(TAG, "BLE stop failed; sleep aborted this round");
    deferSleep(nowMs());
    return;
  }
  ESP_LOGI(TAG, "entering deep sleep");
  eego::FrontlightManager::instance().off();
  NetStack::instance().wifi().powerOff();

  if (deps_.boot.displayReady && deps_.canvas != nullptr) {
    eego::PortraitCanvas& c = *deps_.canvas;
    c.clear();
    c.useUiContentRect();
    char wake[48];
    const uint64_t wakeTs = nowUnix() + static_cast<uint64_t>(sleepMs / 1000);
    struct tm tm_buf;
    localTm(settings_, wakeTs, tm_buf);
    snprintf(wake, sizeof(wake), "%02d-%02d %02d:%02d", tm_buf.tm_mon + 1, tm_buf.tm_mday,
             tm_buf.tm_hour, tm_buf.tm_min);
    // 标题 + 三行说明，整体在内容区垂直居中，左对齐于 kInnerX。
    const int16_t titleLh = lineHeight(style::kTitle.scale);
    const int16_t captionLh = lineHeight(style::kCaption.scale);
    const int16_t blockH = static_cast<int16_t>(titleLh + 8 + 3 * captionLh);
    int16_t y = static_cast<int16_t>((geom::kContentTop + geom::kContentBottom - blockH) / 2);
    drawTextFit(c, geom::kInnerX, y, geom::kInnerW, tr(Str::Sleeping), style::kTitle.scale,
                style::kTitle.bold, style::kTitle.gray);
    y = static_cast<int16_t>(y + titleLh + 8);
    drawTextFit(c, geom::kInnerX, y, geom::kInnerW, tr(Str::WakeHint), style::kCaption.scale,
                style::kCaption.bold, style::kCaption.gray);
    y = static_cast<int16_t>(y + captionLh);
    drawTextFit(c, geom::kInnerX, y, geom::kInnerW, wake, style::kCaption.scale,
                style::kCaption.bold, style::kCaption.gray);
    y = static_cast<int16_t>(y + captionLh);
    drawTextFit(c, geom::kInnerX, y, geom::kInnerW, tr(Str::DeepSleepOffline),
                style::kCaption.scale, style::kCaption.bold, style::kCaption.gray);
    c.requestFullRefresh();
    c.display(eego::RefreshMode::Full);
    c.waitRefreshComplete();
    if (deps_.display != nullptr) deps_.display->deepSleep();
  }

  gpio_hold_en(static_cast<gpio_num_t>(eego::POWER_LATCH));
  gpio_deep_sleep_hold_en();
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(sleepMs) * 1000ULL);
  esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(eego::BUTTON_POWER), 1);

  ESP_LOGI(TAG, "deep sleep %lld ms (timer + power key)", static_cast<long long>(sleepMs));
  esp_deep_sleep_start();
  __builtin_unreachable();
}

// ── 配网窗口 ────────────────────────────────────────────────────────────────

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

// ── 数据辅助 ────────────────────────────────────────────────────────────────

void PanelApp::refreshBattery(int64_t now) {
  if (now - lastBatMs_ < 60000) return;
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
  // 统一时间源（M21）：与 net_task ingest/expire/快照 generated_at 同一墙钟
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
    // 从未发生（如源从未成功）：如实显示"从未"，而不是"刚刚"
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
    case Page::Status: return 2;
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
      out[1] = {tr(Str::Back), true};
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

// ── 告警详情 ────────────────────────────────────────────────────────────────

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
  // 目标已消失（静音/删除/淘汰）：停在原位置的相邻条目。
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
  ++pageEpoch_;  // 布防中的确认绑定旧条目
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

// ── 渲染 ────────────────────────────────────────────────────────────────────

void PanelApp::render(int64_t now) {
  if (page_ == Page::Dashboard) {
    dashboard_.render();
    noteInputResume();
    forceFull_ = false;
    dirty_ = false;
    lastRenderMs_ = now;
    return;
  }
  // 无锁取最新快照（发布方 net_task；代际变化即脏）
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
      snprintf(m.windowText, sizeof(m.windowText), tr(Str::ProvWindowFmt),
               static_cast<unsigned>(admin::ProvisionPortal::instance().windowRemainingMs() /
                                     1000));
      drawProvision(c, m);
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

  c.display(forceFull_ ? eego::RefreshMode::Full : eego::RefreshMode::Fast);
  c.waitRefreshComplete();
  noteInputResume();
  if (forceFull_) lastFullRenderMs_ = now;
  forceFull_ = false;
  dirty_ = false;
  lastRenderMs_ = now;
}

void PanelApp::buildStatusBar(StatusBarModel& m) {
  // UTC 时间源不变；状态栏按 DeviceSettings 的显示时区偏移。
  renderedClockKey_ = -1;
  lastClockCheckMs_ = nowMs();
  if (TimeSource::valid()) {
    const uint64_t now = TimeSource::now();
    renderedClockKey_ = static_cast<int64_t>(now / 60);
    struct tm tm_buf;
    localTm(settings_, now, tm_buf);
    snprintf(m.timeText, sizeof(m.timeText), "%02d-%02d %02d:%02d", tm_buf.tm_mon + 1,
             tm_buf.tm_mday, tm_buf.tm_hour, tm_buf.tm_min);
  }
  m.batPercent = batCache_.percent;
  m.batMv = batCache_.millivolts;
  m.charging = charging_ || batCache_.charging;
  m.wifiState = wifiState_.load();
  m.mains = mains_;
  m.portalOpen = admin::ProvisionPortal::instance().isOpen();
  m.apFallback = admin::ProvisionPortal::instance().isApFallbackActive();
  // 已静音告警数（store 语义：快照内 muted 记录计数，与 S0/列表过滤一致）
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
    m.url = admin::ProvisionPortal::kApUrl;
    snprintf(buf, sizeof(buf), "%s · %s", tr(Str::ApFallback), admin::ProvisionPortal::kApSsid);
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
  // meta 值与 title/message 同为不可信字段（来自数据源/推送客户端），统一钳制
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
  auto section = [&rows](Str label) { rows.emplace_back(tr(label), ""); };
  char buf[128];

  // 网络
  section(Str::SecNetwork);
  std::string wifi = wifiStateText(ns.wifi.state);
  if (!ns.wifi.ssid.empty()) wifi += " · " + ns.wifi.ssid;
  add(tr(Str::LblWifi), wifi);
  if (ns.wifi.state == 2) {
    snprintf(buf, sizeof(buf), "%d dBm", ns.wifi.rssi);
    add(tr(Str::LblSignal), buf);
  }
  if (!ns.wifi.ip.empty()) {
    add(tr(Str::LblAddress), "http://" + ns.wifi.ip + "/");
  }
  if (admin::ProvisionPortal::instance().apActive()) {
    snprintf(buf, sizeof(buf), "%s · %s", admin::ProvisionPortal::kApSsid,
             admin::ProvisionPortal::kApUrl);
    add(tr(Str::ApFallback), buf);
  }
  if (ns.time.valid) {
    if (ns.time.sntp_synced) {
      char sync_at[40];
      formatTs(ns.time.last_sync_at, sync_at, sizeof(sync_at));
      snprintf(buf, sizeof(buf), "SNTP · %s", sync_at);
    } else {
      snprintf(buf, sizeof(buf), "%s", tr(Str::TimeRtc));
    }
  } else {
    snprintf(buf, sizeof(buf), "%s", tr(Str::TimeUnavailable));
  }
  add(tr(Str::TimeLabel), buf);

  // 设备
  section(Str::SecDevice);
  const bool charging = charging_ || batCache_.charging;
  if (batCache_.millivolts > 0) {
    snprintf(buf, sizeof(buf), "%u%% · %umV%s%s", static_cast<unsigned>(batCache_.percent),
             static_cast<unsigned>(batCache_.millivolts), charging ? " · " : "",
             charging ? tr(Str::Charging) : "");
  } else {
    snprintf(buf, sizeof(buf), "%s", tr(Str::ModeMains));
  }
  add(tr(Str::LblBattery), buf);
  const char* pmode = tr(Str::ModeAuto);
  if (settings_.power_mode == PowerMode::Mains) pmode = tr(Str::ModeMains);
  if (settings_.power_mode == PowerMode::Battery) pmode = tr(Str::ModeBattery);
  snprintf(buf, sizeof(buf), "%s → %s", pmode, mains_ ? tr(Str::ModeMains) : tr(Str::ModeBattery));
  add(tr(Str::LblPower), buf);
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

  // 数据源（dashboard 是唯一取数模型）
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

  // 外部告警（POST /alerts 推送）
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

  statusRows_ = rows.size();
  const size_t pages = statusPageCount();
  if (statusPage_ >= pages) statusPage_ = pages - 1;
  const size_t per = geom::kStatusRowsPerPage;
  const size_t begin = statusPage_ * per;
  const size_t end = std::min(begin + per, rows.size());
  for (size_t i = begin; i < end; ++i) m.rows.push_back(std::move(rows[i]));
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
  add(Str::HelpDanger, Str::HelpFreshHold);
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

  // 只为当前页构建行（避免对不可见行做 net/BLE 查询）
  char buf[96];
  const eego::ble::Status ble_runtime = eego::ble::BleManager::instance().status();
  for (size_t i = begin; i < end; i++) {
    S4RowModel row;
    std::string value;
    Str label = Str::None;
    bool isAction = false;
    switch (ids[i]) {
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
          snprintf(buf, sizeof(buf), "%us",
                   static_cast<unsigned>(
                       admin::ProvisionPortal::instance().windowRemainingMs() / 1000));
          value = buf;
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
        } else if (ble_runtime.role == eego::ble::Role::Provisioning) {
          snprintf(buf, sizeof(buf), "%s  %us", blePop_.c_str(),
                   static_cast<unsigned>(ble_runtime.provisioning_remaining_s));
          value = buf;
          isAction = true;
        } else {
          value = tr(Str::Off);
          isAction = true;
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
      case SettingId::PowerMode:
        label = Str::SetPowerMode;
        value = tr(settings_.power_mode == PowerMode::Mains     ? Str::ModeMains
                   : settings_.power_mode == PowerMode::Battery ? Str::ModeBattery
                                                                : Str::ModeAuto);
        break;
      case SettingId::BatteryPoll:
        label = Str::SetBatteryPoll;
        formatEvery(settings_.battery_poll_s, buf, sizeof(buf));
        value = buf;
        break;
      case SettingId::IdleSleep:
        label = Str::SetIdleSleep;
        if (settings_.idle_sleep_s == 0) {
          value = tr(Str::Off);
        } else {
          formatEvery(settings_.idle_sleep_s, buf, sizeof(buf));
          value = buf;
        }
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
        const uint64_t wake = nextWakeAt(nowMs());
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
        Str wake_reason = Str::WakeOther;
        if (wakeCause_ == ESP_SLEEP_WAKEUP_TIMER) wake_reason = Str::WakeTimer;
        else if (wakeCause_ == ESP_SLEEP_WAKEUP_EXT0) wake_reason = Str::WakeButton;
        else if (wakeCause_ == ESP_SLEEP_WAKEUP_UNDEFINED) wake_reason = Str::WakeReset;
        value = tr(wake_reason);
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
      case SettingId::BleObserver:
        label = Str::SetBleObserver;
        if (!ble_runtime.compiled) {
          value = tr(Str::Unavailable);
        } else if (ble_runtime.role == eego::ble::Role::Observer) {
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
    row.label = tr(label);
    row.value = std::move(value);
    row.isAction = isAction;
    m.rows.push_back(std::move(row));
  }
}

}  // namespace panel
