// SPDX-License-Identifier: GPL-3.0-or-later
// 告警面板应用状态机实现（契约见 panel_app.h）。
#include "panel_app.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "admin/admin.h"
#include "admin/provision_portal.h"
#include "alert_core/types.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_log.h"
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
constexpr uint32_t kNetBitSource = 1u << 1;
constexpr uint32_t kPollWaitMs = 45000;
constexpr uint32_t kRenderThrottleMs = 250;
constexpr uint32_t kClockRenderMs = 30000;
constexpr size_t kSettingsRowCount = 28;
constexpr size_t kSettingsPageRows = 8;

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
  page_ = dashboardStartupPage(page_, dashboard_.begin(dashboardDeps));
  dashboard_.setMains(mains_);

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
    eego::FrontlightManager& frontlight = eego::FrontlightManager::instance();
    if (!frontlight.present()) {
      frontlight.off();
    } else {
      frontlight.setWarm(settings_.frontlight_warmth);
      frontlight.setBrightness(settings_.frontlight_enabled
                                   ? settings_.frontlight_brightness
                                   : 0);
    }
  }
  if ((changed & eego::settings::kChangedDataRefresh) != 0) {
    const uint32_t interval_ms = settings_.data_refresh_s * 1000;
    if (!NetStack::instance().setDataRefreshInterval(interval_ms)) {
      ESP_LOGW(TAG, "data refresh scheduler unavailable");
    }
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
  if (admin::ProvisionPortal::instance().apActive()) return 0;
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

void PanelApp::notifyNetEvent(int wifiState, bool sourceHealthChanged) {
  wifiState_.store(wifiState);
  uint32_t bits = kNetBitWifi;
  if (sourceHealthChanged) bits |= kNetBitSource;
  netBits_.fetch_or(bits);
}

void PanelApp::notifyAdminEvent(int type, int reason, uint32_t remainingS) {
  if (adminQueue_ == nullptr) return;
  AdminMail mail{type, reason, remainingS};
  xQueueSend(static_cast<QueueHandle_t>(adminQueue_), &mail, 0);
}

void PanelApp::drainNetBits() {
  const uint32_t bits = netBits_.exchange(0);
  if (bits == 0) return;
  markDirty();
  if ((bits & kNetBitSource) != 0) netStatusCacheMs_ = 0;  // 源健康跃迁 → 立即刷新缓存
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
        showBanner(tr(Str::ApFallback), 5000);
        break;
      case kEvApFallbackClosed:
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

  // 快照代际跟踪（无锁 acquire；渲染在 render() 统一取）
  if (deps_.publisher != nullptr) {
    auto snap = deps_.publisher->acquire();
    if (snap && snap->seq != snapshotSeq_) markDirty();
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

  // 详情目标消失（过期/淘汰）→ 退回列表
  if (page_ == Page::Detail && findRecord(detailId_) == nullptr) {
    back();
  }

  const bool wantMains = computeMains();
  if (wantMains != mains_) onModeChange(wantMains);
  dashboard_.service(mains_);
  if (page_ == Page::Dashboard && dashboard_.consumeExitRequest()) gotoPage(Page::Overview);

  if (now - lastScheduleCheckMs_ > 1000) {
    lastScheduleCheckMs_ = now;
    if (!admin::ProvisionPortal::instance().apActive()) {
      uint64_t wake_at = 0;
      uint32_t remaining = 0;
      if (eego::settings::scheduledSleep(settings_, nowUnix(), wake_at, remaining)) {
        enterSleep(now, static_cast<int64_t>(remaining) * 1000);
      }
    }
  }
  batteryFsm(now);

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
      if (forceFull_) lastFullRenderMs_ = now;
      dirty_ = false;
      forceFull_ = false;
      lastRenderMs_ = now;
    }
    return;
  }

  if (now - lastRenderMs_ > kClockRenderMs) markDirty();

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

void PanelApp::handleButton(const eego::ButtonEvent& ev) {
  const int64_t now_us = esp_timer_get_time();
  if (ev.timestamp_us <= 0 || now_us < ev.timestamp_us ||
      now_us - ev.timestamp_us > eego::kInputActionMaxAgeUs) {
    return;
  }
  lastInputMs_ = now_us / 1000;
  const size_t index = static_cast<size_t>(ev.id);
  if (index >= 3) return;
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
      const std::string control = wanted.rfind("trigger:", 0) == 0
                                      ? buttonTriggerControl(ev.id)
                                      : focusedControl();
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
  if (ev.timestamp_us <= 0 || now_us < ev.timestamp_us ||
      now_us - ev.timestamp_us > eego::kInputActionMaxAgeUs) {
    return;
  }
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
      if (confirm_ != ConfirmKind::None) clearConfirm();
      requestNavigation(NavigationKind::Page,
                        ev.type == eego::TouchEventType::SwipeLeft ? 1 : -1, now_us);
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
      onFrontKeyShort();
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
    gotoPage(Page::Overview);
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

void PanelApp::onFrontKeyShort() {
  if (page_ == Page::Overview) {
    gotoPage(Page::Table);
  } else {
    gotoPage(Page::Overview);
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
    case Page::Overview: return "overview";
    case Page::Table: return "table";
    case Page::Detail: return "detail";
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

void PanelApp::movePage(int delta) {
  switch (page_) {
    case Page::Table: {
      std::vector<const alert_core::AlertRecord*> visible;
      const size_t total = visibleRecords(visible);
      const size_t pages = std::max<size_t>(1, (total + geom::kRowsPerPage - 1) /
                                                   geom::kRowsPerPage);
      int64_t next = (static_cast<int64_t>(tablePage_) + delta) % static_cast<int64_t>(pages);
      if (next < 0) next += static_cast<int64_t>(pages);
      tablePage_ = static_cast<size_t>(next);
      focus_ = 0;
      markDirty(true);
      break;
    }
    case Page::Settings: {
      const size_t pages = (kSettingsRowCount + kSettingsPageRows - 1) / kSettingsPageRows;
      const size_t current = focus_ / kSettingsPageRows;
      int64_t signed_next = (static_cast<int64_t>(current) + delta) % static_cast<int64_t>(pages);
      if (signed_next < 0) signed_next += static_cast<int64_t>(pages);
      const size_t next = static_cast<size_t>(signed_next);
      focus_ = std::min(next * kSettingsPageRows, kSettingsRowCount - 1);
      markDirty(true);
      break;
    }
    case Page::Dashboard:
      dashboard_.movePage(delta);
      break;
    case Page::Overview:
      if (delta > 0) gotoPage(Page::Table);
      break;
    default:
      break;
  }
}

// ── 导航 ────────────────────────────────────────────────────────────────────

namespace {

size_t wrappedIndex(size_t current, int delta, size_t count) {
  if (count == 0) return 0;
  int64_t next = (static_cast<int64_t>(current) + delta) % static_cast<int64_t>(count);
  if (next < 0) next += static_cast<int64_t>(count);
  return static_cast<size_t>(next);
}

}  // namespace

void PanelApp::gotoPage(Page p) {
  if (page_ == p || (p == Page::Dashboard && !dashboard_.available())) return;
  page_ = p;
  ++pageEpoch_;
  navigation_.cancel();
  focus_ = 0;
  provisionSucceeded_ = false;
  clearConfirm();
  if (p == Page::Settings) refreshMuteRuleCount();
  if (p == Page::Dashboard) dashboard_.invalidateAll();
  markDirty(true);
}

void PanelApp::back() {
  switch (page_) {
    case Page::Detail: {
      const std::string target = detailId_;
      gotoPage(Page::Table);
      std::vector<const alert_core::AlertRecord*> vis;
      visibleRecords(vis);
      for (size_t i = 0; i < vis.size(); i++) {
        if (vis[i]->id == target) {
          tablePage_ = i / geom::kRowsPerPage;
          focus_ = i % geom::kRowsPerPage;
          break;
        }
      }
      markDirty();
      break;
    }
    case Page::Help:
      gotoPage(helpReturn_ == Page::Help ? Page::Overview : helpReturn_);
      break;
    case Page::Table:
    case Page::Status:
    case Page::Settings:
      gotoPage(Page::Overview);
      break;
    case Page::Dashboard:
      if (!dashboard_.back()) gotoPage(Page::Overview);
      break;
    default:
      break;
  }
}

void PanelApp::moveFocus(int delta) {
  switch (page_) {
    case Page::Overview:
    case Page::Detail:
    case Page::Status:
    case Page::Help:
      focus_ = wrappedIndex(focus_, delta, softKeyCount());
      markDirty();
      break;
    case Page::Table: {
      std::vector<const alert_core::AlertRecord*> vis;
      const size_t total = visibleRecords(vis);
      if (total == 0) break;
      int64_t idx = static_cast<int64_t>(tablePage_ * geom::kRowsPerPage + focus_) + delta;
      idx = std::max<int64_t>(0, std::min<int64_t>(idx, static_cast<int64_t>(total) - 1));
      tablePage_ = static_cast<size_t>(idx) / geom::kRowsPerPage;
      focus_ = static_cast<size_t>(idx) % geom::kRowsPerPage;
      markDirty();
      break;
    }
    case Page::Settings:
      focus_ = wrappedIndex(focus_, delta, kSettingsRowCount);
      markDirty();
      break;
    case Page::Dashboard:
      dashboard_.moveFocus(delta, false);
      break;
    default:
      break;
  }
}

void PanelApp::activateFocused() {
  switch (page_) {
    case Page::Overview:
    case Page::Detail:
    case Page::Status:
    case Page::Help:
      activateSoftKey(focus_);
      break;
    case Page::Table: {
      std::vector<const alert_core::AlertRecord*> vis;
      visibleRecords(vis);
      const size_t idx = tablePage_ * geom::kRowsPerPage + focus_;
      if (idx < vis.size()) {
        detailId_ = vis[idx]->id;
        gotoPage(Page::Detail);
      }
      break;
    }
    case Page::Settings:
      activateSoftKey(focus_ + 1);
      break;
    case Page::Dashboard:
      dashboard_.activate();
      break;
    default:
      break;
  }
}

void PanelApp::activateSoftKey(size_t idx) {
  switch (page_) {
    case Page::Overview:
      if (idx == 0) gotoPage(Page::Table);
      else if (idx == 1) armConfirm(ConfirmKind::AckAll);
      else if (idx == 2) gotoPage(Page::Settings);
      else if (idx == 3) gotoPage(Page::Dashboard);
      break;
    case Page::Table:
      if (idx == 0) activateFocused();
      else if (idx == 1) back();
      break;
    case Page::Detail: {
      const alert_core::AlertRecord* rec = findRecord(detailId_);
      if (idx == 0 && rec != nullptr && rec->state == alert_core::AlertState::Active) {
        doAck(detailId_);
      } else if (idx == 1 && rec != nullptr) {
        armConfirm(ConfirmKind::Mute);
      } else if (idx == 2 && rec != nullptr &&
                 (rec->state == alert_core::AlertState::Active ||
                  rec->state == alert_core::AlertState::Acked)) {
        armConfirm(ConfirmKind::Clear);
      } else if (idx == 3 && rec != nullptr &&
                 !dashboard_.linkedPanelForAlert(*rec).empty()) {
        const std::string linked = dashboard_.linkedPanelForAlert(*rec);
        if (dashboard_.gotoPanel(linked)) gotoPage(Page::Dashboard);
      } else if (idx == 3 || idx == 4) {
        back();
      }
      break;
    }
    case Page::Status:
      if (idx == 0) doPollNow();
      else if (idx == 1) back();
      break;
    case Page::Settings:
      if (idx == 0) back();
      else activateSettingRow(idx - 1);
      break;
    case Page::Help:
      if (idx == 0) back();
      break;
    default:
      break;
  }
}

// ── 触控 ────────────────────────────────────────────────────────────────────

void PanelApp::resolveTouchHit(int16_t x, int16_t y, int& hit_idx, bool& hit_row) const {
  hit_idx = -1;
  hit_row = false;
  if (y >= geom::kSoftKeyY && y <= geom::kSoftKeyY + geom::kSoftKeyH) {
    const size_t n = softKeyCount();
    if (n > 0) {
      const int16_t gap = 8;
      const int16_t kw = static_cast<int16_t>((geom::kContentW - gap * (n - 1)) / n);
      int idx = (x - geom::kContentX) / (kw + gap);
      idx = std::max(0, std::min(idx, static_cast<int>(n) - 1));
      hit_idx = idx;
    }
  } else if (page_ == Page::Settings) {
    const int16_t rows_y = geom::kContentTop + 64;
    if (y >= rows_y && y < rows_y + static_cast<int16_t>(kSettingsPageRows * 56)) {
      const size_t row = static_cast<size_t>((y - rows_y) / 56);
      const size_t global = (focus_ / kSettingsPageRows) * kSettingsPageRows + row;
      if (global < kSettingsRowCount) {
        hit_row = true;
        hit_idx = static_cast<int>(global + 1);
      }
    }
  }
}

std::string PanelApp::controlForHit(int hit_idx, bool hit_row) const {
  if (hit_idx < 0) return {};
  return std::to_string(static_cast<unsigned>(page_)) + (hit_row ? ":row:" : ":key:") +
         std::to_string(hit_idx);
}

std::string PanelApp::focusedControl() const {
  if (page_ == Page::Dashboard) return dashboard_.focusedControl();
  if (page_ == Page::Settings) return controlForHit(static_cast<int>(focus_ + 1), true);
  if (page_ == Page::Overview || page_ == Page::Detail || page_ == Page::Status ||
      page_ == Page::Help) {
    return controlForHit(static_cast<int>(focus_), false);
  }
  return {};
}

std::string PanelApp::touchControl(int16_t x, int16_t y) const {
  if (page_ == Page::Dashboard) return dashboard_.touchControl(x, y);
  int hit_idx = -1;
  bool hit_row = false;
  resolveTouchHit(x, y, hit_idx, hit_row);
  return controlForHit(hit_idx, hit_row);
}

void PanelApp::onTouchTap(int16_t x, int16_t y) {
  if (page_ == Page::Dashboard) {
    dashboard_.touch(x, y);
    return;
  }
  int hit_idx = -1;
  bool hit_row = false;
  resolveTouchHit(x, y, hit_idx, hit_row);
  if (hit_idx >= 0 && !hit_row) {
    if (page_ == Page::Settings) back();
    else activateSoftKey(static_cast<size_t>(hit_idx));
    return;
  }
  if (hit_row) {
    const size_t row = static_cast<size_t>(hit_idx - 1);
    if (row == focus_) activateSoftKey(static_cast<size_t>(hit_idx));
    else {
      focus_ = row;
      markDirty();
    }
    return;
  }
  switch (page_) {
    case Page::Overview:
      if (y >= geom::kContentTop + 64 && y < geom::kContentTop + 474) gotoPage(Page::Table);
      break;
    case Page::Table: {
      const int16_t rows_y = geom::kContentTop + 60;
      if (y >= rows_y && y < rows_y + geom::kRowsPerPage * geom::kRowH) {
        const size_t row = static_cast<size_t>((y - rows_y) / geom::kRowH);
        std::vector<const alert_core::AlertRecord*> vis;
        visibleRecords(vis);
        if (tablePage_ * geom::kRowsPerPage + row < vis.size()) {
          focus_ = row;
          detailId_ = vis[tablePage_ * geom::kRowsPerPage + row]->id;
          gotoPage(Page::Detail);
        }
      }
      break;
    }
    default:
      break;
  }
}

// ── 动作 ────────────────────────────────────────────────────────────────────

void PanelApp::doAck(const std::string& id) {
  AlertActionResult r = NetStack::instance().alertAction({AlertActionType::Ack, id, "panel"});
  if (r.code == AlertActionResult::Code::Ok) {
    showBanner(tr(Str::DoneAck), 3000);
  } else {
    showBannerFmt(4000, "%s: %s", tr(Str::ActionFailed), r.detail.c_str());
  }
  markDirty();
}

void PanelApp::doAckAll() {
  AlertActionResult r =
      NetStack::instance().alertAction({AlertActionType::AckAll, "", "panel"});
  if (r.code == AlertActionResult::Code::Ok) {
    showBannerFmt(3000, "%s x%u", tr(Str::DoneAck), static_cast<unsigned>(r.affected));
  } else {
    showBannerFmt(4000, "%s: %s", tr(Str::ActionFailed), r.detail.c_str());
  }
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
  if (page_ == Page::Detail) back();  // 静音后详情目标从列表消失，回列表
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
  NetStack::instance().pollNow("");
  showBanner(tr(Str::PollNowStarted), 3000);
  markDirty();
}

void PanelApp::activateSettingRow(size_t row) {
  eego::settings::Settings next = settings_;
  switch (row) {
    case 0: {
      const eego::net::NetStatusSnapshot& status = netStatus();
      if (!status.wifi.ip.empty()) showBannerFmt(5000, "http://%s/", status.wifi.ip.c_str());
      else showBanner(tr(Str::WifiDisconnected), 3000);
      break;
    }
    case 1: armConfirm(ConfirmKind::NetworkRecovery); break;
    case 2: armConfirm(ConfirmKind::WifiReset); break;
    case 3: showBanner(tr(Str::Unavailable), 3000); break;
    case 4:
      next.language = settings_.language == "zh" ? "en" : "zh";
      updateSettings(next);
      break;
    case 5:
      next.timezone_offset_min += 60;
      if (next.timezone_offset_min > 840) next.timezone_offset_min = -720;
      updateSettings(next);
      break;
    case 6:
    case 7:
    case 8: {
      if (!eego::FrontlightManager::instance().present()) {
        showBanner(tr(Str::FrontlightMissing), 3000);
        break;
      }
      if (row == 6) next.frontlight_enabled = !settings_.frontlight_enabled;
      if (row == 7) {
        switch (settings_.frontlight_brightness) {
          case 10: next.frontlight_brightness = 25; break;
          case 25: next.frontlight_brightness = 50; break;
          case 50: next.frontlight_brightness = 75; break;
          case 75: next.frontlight_brightness = 100; break;
          default: next.frontlight_brightness = 10; break;
        }
      }
      if (row == 8) next.frontlight_warmth = (settings_.frontlight_warmth + 25) % 125;
      updateSettings(next);
      break;
    }
    case 9:
      switch (settings_.data_refresh_s) {
        case 0: next.data_refresh_s = 60; break;
        case 60: next.data_refresh_s = 300; break;
        case 300: next.data_refresh_s = 900; break;
        default: next.data_refresh_s = 0; break;
      }
      updateSettings(next);
      break;
    case 10: doPollNow(); break;
    case 11:
      switch (settings_.epd_full_refresh_s) {
        case 0: next.epd_full_refresh_s = 3600; break;
        case 3600: next.epd_full_refresh_s = 86400; break;
        case 86400: next.epd_full_refresh_s = 604800; break;
        default: next.epd_full_refresh_s = 0; break;
      }
      updateSettings(next);
      break;
    case 12:
      markDirty(true);
      showBanner(tr(Str::FullRefreshQueued), 3000);
      break;
    case 13:
      switch (settings_.power_mode) {
        case PowerMode::Auto: next.power_mode = PowerMode::Mains; break;
        case PowerMode::Mains: next.power_mode = PowerMode::Battery; break;
        default: next.power_mode = PowerMode::Auto; break;
      }
      updateSettings(next);
      break;
    case 14:
      switch (settings_.battery_poll_s) {
        case 300: next.battery_poll_s = 900; break;
        case 900: next.battery_poll_s = 1800; break;
        case 1800: next.battery_poll_s = 3600; break;
        case 3600: next.battery_poll_s = 21600; break;
        default: next.battery_poll_s = 300; break;
      }
      updateSettings(next);
      break;
    case 15:
      switch (settings_.idle_sleep_s) {
        case 0: next.idle_sleep_s = 30; break;
        case 30: next.idle_sleep_s = 90; break;
        case 90: next.idle_sleep_s = 300; break;
        case 300: next.idle_sleep_s = 600; break;
        default: next.idle_sleep_s = 0; break;
      }
      updateSettings(next);
      break;
    case 16:
      if (settings_.sleep_start_min < 0) {
        next.sleep_start_min = 22 * 60;
        next.sleep_end_min = 6 * 60;
      } else if (settings_.sleep_start_min == 22 * 60 && settings_.sleep_end_min == 6 * 60) {
        next.sleep_start_min = 23 * 60;
        next.sleep_end_min = 7 * 60;
      } else if (settings_.sleep_start_min == 23 * 60 && settings_.sleep_end_min == 7 * 60) {
        next.sleep_start_min = 12 * 60 + 30;
        next.sleep_end_min = 13 * 60 + 30;
      } else {
        next.sleep_start_min = -1;
        next.sleep_end_min = -1;
      }
      if (updateSettings(next) && next.sleep_start_min >= 0) {
        showBanner(tr(Str::DeepSleepOffline), 5000);
      }
      break;
    case 17: {
      const uint64_t wake = nextWakeAt(nowMs());
      if (wake == 0) showBanner(tr(Str::NotScheduled), 3000);
      else {
        char text[32];
        formatTs(wake, text, sizeof(text));
        showBanner(text, 4000);
      }
      break;
    }
    case 18: {
      Str text = Str::WakeOther;
      if (wakeCause_ == ESP_SLEEP_WAKEUP_TIMER) text = Str::WakeTimer;
      else if (wakeCause_ == ESP_SLEEP_WAKEUP_EXT0) text = Str::WakeButton;
      else if (wakeCause_ == ESP_SLEEP_WAKEUP_UNDEFINED) text = Str::WakeReset;
      showBanner(tr(text), 3000);
      break;
    }
    case 19:
      showBanner(eego::SdCard::instance().ready() ? tr(Str::SdReady) : tr(Str::SdMissing), 3000);
      break;
    case 20:
      next.diagnostics_enabled = !settings_.diagnostics_enabled;
      updateSettings(next);
      break;
    case 21:
      if (settings_.diagnostics_enabled) diagSmoke();
      else showBanner(tr(Str::Unavailable), 3000);
      break;
    case 22:
      showBanner(admin::AuthManager::instance().hasPassword() ? tr(Str::SecuritySet)
                                                              : tr(Str::SecurityMissing),
                 3000);
      break;
    case 23: armConfirm(ConfirmKind::ClearMutes); break;
    case 24: armConfirm(ConfirmKind::FactoryReset); break;
    case 25: {
      const esp_app_desc_t* description = esp_app_get_description();
      showBannerFmt(5000, "%s %s", description->project_name, description->version);
      break;
    }
    case 26:
      helpReturn_ = Page::Settings;
      gotoPage(Page::Help);
      break;
    case 27: resetInputBindings(); break;
    default: break;
  }
}

void PanelApp::doOpenRecovery() {
  esp_err_t err = ESP_OK;
  if (!admin::ProvisionPortal::instance().apActive()) {
    err = admin::ProvisionPortal::instance().openApFallback();
  }
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    showBannerFmt(4000, "%s: %s", tr(Str::ActionFailed), esp_err_to_name(err));
    return;
  }
  awakeDeadlineMs_ = nowMs() + 600000;
  showBanner(tr(Str::RecoveryApStarted), 7000);
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

void PanelApp::resetInputBindings() {
  std::string error;
  bool conflict = false;
  const auto document = admin::DashboardStore::instance().snapshot();
  const eego::dashboard::Dashboard* current =
      document != nullptr && document->configured ? &document->dashboard : nullptr;
  if (admin::InputBindings::instance().reset(false, 0, current, error, conflict)) {
    showBanner(tr(Str::InputResetDone), 3000);
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
  if (confirm_ == ConfirmKind::AckAll || confirm_ == ConfirmKind::Mute ||
      confirm_ == ConfirmKind::Clear) {
    const auto latest = deps_.publisher != nullptr ? deps_.publisher->acquire() : snapshot_;
    const ConfirmRecordRule rule = confirm_ == ConfirmKind::AckAll
                                       ? ConfirmRecordRule::AckAll
                                       : (confirm_ == ConfirmKind::Mute ? ConfirmRecordRule::Mute
                                                                       : ConfirmRecordRule::Clear);
    if (!confirmRecordTargetCurrent(target, rule, latest.get())) return false;
    if (confirm_ != ConfirmKind::AckAll &&
        (page_ != Page::Detail || detailId_ != target.record_id)) {
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
    case ConfirmKind::AckAll: doAckAll(); break;
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
  if (kind == ConfirmKind::AckAll || kind == ConfirmKind::Mute || kind == ConfirmKind::Clear) {
    target.snapshot_epoch = currentSnapshotEpoch();
  }
  if (kind == ConfirmKind::Mute || kind == ConfirmKind::Clear) target.record_id = detailId_;
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

void PanelApp::batteryFsm(int64_t now) {
  if (admin::ProvisionPortal::instance().apActive()) {
    if (battPhase_ == BattPhase::Serving) awakeDeadlineMs_ = now + 60000;
    return;
  }
  switch (battPhase_) {
    case BattPhase::Mains:
      return;
    case BattPhase::Polling: {
      if (!batteryPollStarted_) {
        if (wifiState_.load() == 2 /*Connected*/ || now > pollDeadlineMs_) {
          ESP_LOGI(TAG, "battery poll cycle: poll all sources");
          NetStack::instance().pollNow("");
          dashboard_.pollBattery();
          batteryPollStarted_ = true;
          pollDeadlineMs_ = now + kDashboardPollWaitMs;
          markDirty(true);
        }
        return;
      }

      const bool dashboard_idle = dashboard_.networkIdle();
      if (!dashboard_idle && now <= pollDeadlineMs_) return;
      if (!dashboard_idle) {
        ESP_LOGW(TAG, "battery poll: dashboard still busy, serving cached data");
      }
      if (!NetStack::instance().waitIdle(30000)) {
        ESP_LOGW(TAG, "battery poll: waitIdle timeout, retrying once");
        if (!NetStack::instance().waitIdle(10000)) {
          ESP_LOGW(TAG, "battery poll: net still not idle, serving with current data");
        }
      }
      battPhase_ = BattPhase::Serving;
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

void PanelApp::enterSleep(int64_t now, int64_t durationMs) {
  ESP_LOGI(TAG, "entering deep sleep");
  int64_t sleepMs = durationMs;
  if (sleepMs < 0) {
    sleepMs = cycleStartMs_ + static_cast<int64_t>(settings_.battery_poll_s) * 1000 - now;
  }
  if (sleepMs < 10000) sleepMs = 10000;

  if (!dashboard_.networkIdle()) {
    ESP_LOGW(TAG, "battery: dashboard work pending, sleep deferred");
    battPhase_ = mains_ ? BattPhase::Mains : BattPhase::Serving;
    awakeDeadlineMs_ = now + 30000;
    lastScheduleCheckMs_ = now + 30000;
    applySettings(eego::settings::kChangedFrontlight);
    markDirty();
    return;
  }

  // 1. 网络静默（显式处理超时：重试一轮；仍超时则中止本次休眠回 Serving，
  //    30s 后重试——绝不带在途 HTTP 传输断电 WiFi）
  if (!NetStack::instance().waitIdle(10000) &&
      !NetStack::instance().waitIdle(5000)) {
    ESP_LOGW(TAG, "net not idle after retries, sleep aborted this round");
    battPhase_ = mains_ ? BattPhase::Mains : BattPhase::Serving;
    awakeDeadlineMs_ = now + 30000;
    lastScheduleCheckMs_ = now + 30000;
    applySettings(eego::settings::kChangedFrontlight);
    markDirty();
    return;
  }
  eego::FrontlightManager::instance().off();
  NetStack::instance().wifi().powerOff();

  if (deps_.boot.displayReady && deps_.canvas != nullptr) {
    eego::PortraitCanvas& c = *deps_.canvas;
    c.clear();
    c.useUiContentRect();
    eego::FontManager::instance().drawTextGray(c, 76, 320, tr(Str::Sleeping), 2, 0);
    eego::FontManager::instance().drawTextGray(c, 76, 360, tr(Str::WakeHint), 1, 6);
    char wake[48];
    const uint64_t wakeTs = nowUnix() + static_cast<uint64_t>(sleepMs / 1000);
    struct tm tm_buf;
    localTm(settings_, wakeTs, tm_buf);
    snprintf(wake, sizeof(wake), "%02d-%02d %02d:%02d", tm_buf.tm_mon + 1, tm_buf.tm_mday,
             tm_buf.tm_hour, tm_buf.tm_min);
    eego::FontManager::instance().drawTextGray(c, 76, 396, wake, 1, 6);
    eego::FontManager::instance().drawTextGray(c, 76, 420, tr(Str::DeepSleepOffline), 1, 6);
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
  gotoPage(Page::Overview);
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

bool PanelApp::recordVisible(const alert_core::AlertRecord& r) const {
  // store 级静音语义（M21）：muted 记录留在库但不进总览/列表/徽标（M11）；
  // 与 API 默认列表（muted 排除）、状态统计一致——不再有 UI 级指纹过滤。
  return !r.muted;
}

size_t PanelApp::visibleRecords(std::vector<const alert_core::AlertRecord*>& out) const {
  out.clear();
  if (snapshot_ != nullptr) {
    out.reserve(snapshot_->records.size());
    for (const alert_core::AlertRecord& r : snapshot_->records) {
      if (recordVisible(r)) out.push_back(&r);
    }
  }
  return out.size();
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
    case Page::Overview: return dashboard_.available() ? 4 : 3;
    case Page::Table: return 2;
    case Page::Detail: {
      const alert_core::AlertRecord* rec = findRecord(detailId_);
      return rec != nullptr && !dashboard_.linkedPanelForAlert(*rec).empty() ? 5 : 4;
    }
    case Page::Status: return 2;
    case Page::Settings:
    case Page::Help: return 1;
    default: return 0;
  }
}

void PanelApp::softKeys(SoftKeyModel* out, size_t cap) const {
  const size_t n = softKeyCount();
  if (n == 0 || cap < n) return;
  switch (page_) {
    case Page::Overview:
      out[0] = {tr(Str::List), true};
      out[1] = {tr(Str::AckAll), true};
      out[2] = {tr(Str::SettingsKey), true};
      if (n == 4) out[3] = {currentLang() == Lang::Zh ? "面板" : "DASH", true};
      break;
    case Page::Table:
      out[0] = {tr(Str::Open), true};
      out[1] = {tr(Str::Back), true};
      break;
    case Page::Detail: {
      const alert_core::AlertRecord* rec = findRecord(detailId_);
      const bool active = rec != nullptr && rec->state == alert_core::AlertState::Active;
      const bool clearable = rec != nullptr && (rec->state == alert_core::AlertState::Active ||
                                                rec->state == alert_core::AlertState::Acked);
      const bool muted = rec != nullptr && rec->muted;  // store 级（M21）
      const bool trend = rec != nullptr && !dashboard_.linkedPanelForAlert(*rec).empty();
      out[0] = {tr(Str::Ack), active};
      out[1] = {tr(Str::Mute), rec != nullptr && !muted};
      out[2] = {tr(Str::Clear), clearable};
      out[3] = {trend ? (currentLang() == Lang::Zh ? "趋势" : "TREND") : tr(Str::Back), true};
      if (trend) out[4] = {tr(Str::Back), true};
      break;
    }
    case Page::Status:
      out[0] = {tr(Str::Refresh), true};
      out[1] = {tr(Str::Back), true};
      break;
    case Page::Settings:
    case Page::Help:
      out[0] = {tr(Str::Back), true};
      break;
    default:
      break;
  }
}

// ── 渲染 ────────────────────────────────────────────────────────────────────

void PanelApp::render(int64_t now) {
  if (page_ == Page::Dashboard) {
    dashboard_.render();
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
    case Page::Overview: {
      S0Model m;
      buildS0(m);
      drawS0(c, m);
      break;
    }
    case Page::Table: {
      S1Model m;
      buildS1(m);
      drawS1(c, m);
      break;
    }
    case Page::Detail: {
      S2Model m;
      buildS2(m);
      drawS2(c, m);
      break;
    }
    case Page::Status: {
      S3Model m;
      buildS3(m);
      drawS3(c, m);
      break;
    }
    case Page::Settings: {
      S4Model m;
      buildS4(m);
      drawS4(c, m);
      break;
    }
    case Page::Help: {
      S3Model m;
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
  size_t focusIdx = static_cast<size_t>(-1);
  if (page_ == Page::Overview || page_ == Page::Detail || page_ == Page::Status ||
      page_ == Page::Help) {
    focusIdx = focus_;
  }
  drawSoftKeys(c, keys, n, focusIdx);

  c.display(forceFull_ ? eego::RefreshMode::Full : eego::RefreshMode::Fast);
  c.waitRefreshComplete();
  if (forceFull_) lastFullRenderMs_ = now;
  forceFull_ = false;
  dirty_ = false;
  lastRenderMs_ = now;
}

void PanelApp::buildStatusBar(StatusBarModel& m) {
  // UTC 时间源不变；状态栏按 DeviceSettings 的显示时区偏移。
  if (TimeSource::valid()) {
    const uint64_t now = TimeSource::now();
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

void PanelApp::buildS0(S0Model& m) {
  m.critical = m.warning = m.info = 0;
  m.acked = 0;
  size_t storeMuted = 0;
  if (snapshot_ != nullptr) {
    for (const alert_core::AlertRecord& r : snapshot_->records) {
      if (r.muted) {
        storeMuted++;
        continue;
      }
      if (r.state == alert_core::AlertState::Active || r.state == alert_core::AlertState::Acked) {
        switch (r.severity) {
          case alert_core::Severity::Critical: m.critical++; break;
          case alert_core::Severity::Warning: m.warning++; break;
          default: m.info++; break;
        }
        if (r.state == alert_core::AlertState::Acked) m.acked++;
      }
    }
  }
  m.activeTotal = m.critical + m.warning + m.info;
  m.muted = storeMuted;
  m.empty = (m.activeTotal == 0);
  if (snapshot_ != nullptr) formatTs(snapshot_->generated_at, m.updatedText, sizeof(m.updatedText));

  // 源健康（netStatus() 5s 缓存，避免每次渲染都过命令队列）
  const eego::net::NetStatusSnapshot& ns = netStatus();
  m.sourcesTotal = ns.sources.size();
  m.sourcesOk = 0;
  for (const auto& s : ns.sources) {
    if (s.consecutive_failures == 0) m.sourcesOk++;
  }
}

void PanelApp::buildS1(S1Model& m) {
  std::vector<const alert_core::AlertRecord*> vis;
  m.total = visibleRecords(vis);
  m.pageCount = (m.total + geom::kRowsPerPage - 1) / geom::kRowsPerPage;
  if (m.pageCount == 0) m.pageCount = 1;
  if (tablePage_ >= m.pageCount) tablePage_ = m.pageCount - 1;
  m.page = tablePage_;

  const size_t begin = tablePage_ * geom::kRowsPerPage;
  const size_t end = std::min(begin + static_cast<size_t>(geom::kRowsPerPage), m.total);
  if (focus_ >= end - begin) focus_ = (end > begin) ? (end - begin - 1) : 0;
  m.selected = focus_;

  for (size_t i = begin; i < end; i++) {
    const alert_core::AlertRecord& r = *vis[i];
    S1RowModel row;
    row.severity = static_cast<uint8_t>(r.severity);
    row.state = static_cast<uint8_t>(r.state);
    row.title = r.title.empty() ? r.type : r.title;
    sanitizeClamped(row.title, 120);
    std::string tname;
    row.subtitle = typeName(r.type, tname);
    row.subtitle += " · ";
    row.subtitle += r.source.id;
    sanitizeClamped(row.subtitle, 80);
    formatAge(r.last_seen, row.age, sizeof(row.age));
    m.rows.push_back(std::move(row));
  }
}

void PanelApp::buildS2(S2Model& m) {
  const alert_core::AlertRecord* rec = findRecord(detailId_);
  if (rec == nullptr) return;
  m.severity = static_cast<uint8_t>(rec->severity);
  m.state = static_cast<uint8_t>(rec->state);
  std::string tname;
  m.typeName = typeName(rec->type, tname);
  sanitizeClamped(m.typeName, 60);
  m.title = rec->title;
  sanitizeClamped(m.title, 120);
  m.message = rec->message;
  sanitizeClamped(m.message, 600);
  m.canAck = rec->state == alert_core::AlertState::Active;
  m.canClear = rec->state == alert_core::AlertState::Active ||
               rec->state == alert_core::AlertState::Acked;
  m.muted = rec->muted;  // store 级静音（M21）

  char buf[40];
  // meta 值与 title/message 同为不可信字段（ack_by 来自 API 客户端），统一钳制
  auto addMeta = [&m](Str label, std::string value) {
    sanitizeClamped(value, 80);
    m.meta.emplace_back(tr(label), std::move(value));
  };
  addMeta(Str::LblType, rec->type);
  addMeta(Str::LblSource, rec->source.id + "/" + rec->source.kind);
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
}

void PanelApp::buildS3(S3Model& m) {
  const eego::net::NetStatusSnapshot& ns = netStatus();
  m.configValid = admin::ConfigStore::instance().valid();

  auto add = [&m](const std::string& label, const std::string& value) {
    m.rows.emplace_back(label, value);
  };
  auto addSection = [&m](Str label) { m.rows.emplace_back(tr(label), ""); };

  char buf[96];
  // 模式/电源
  const char* pmode = tr(Str::ModeAuto);
  if (settings_.power_mode == PowerMode::Mains) pmode = tr(Str::ModeMains);
  if (settings_.power_mode == PowerMode::Battery) pmode = tr(Str::ModeBattery);
  snprintf(buf, sizeof(buf), "%s -> %s%s", pmode, mains_ ? tr(Str::ModeMains) : tr(Str::ModeBattery),
           (charging_ || batCache_.charging) ? tr(Str::Charging) : "");
  add(tr(Str::SetPowerMode), buf);
  snprintf(buf, sizeof(buf), tr(Str::BatteryFmt), static_cast<unsigned>(batCache_.percent),
           static_cast<unsigned>(batCache_.millivolts));
  add(tr(Str::SysStatus), buf);

  // WiFi
  const char* wst = tr(Str::WifiDisconnected);
  switch (ns.wifi.state) {
    case 1: wst = tr(Str::WifiConnecting); break;
    case 2: wst = tr(Str::WifiConnected); break;
    case 3: wst = tr(Str::WifiFailed); break;
    case 4: wst = tr(Str::WifiOffline); break;
    default: break;
  }
  add("WiFi", wst);
  if (!ns.wifi.ssid.empty()) {
    snprintf(buf, sizeof(buf), "%s  RSSI %d  %s", ns.wifi.ssid.c_str(), ns.wifi.rssi,
             ns.wifi.ip.c_str());
    add("  ", buf);
  }

  // 统一时间源（M21）：如实显示来源与可用性，不可用时不拿 0 时间冒充
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

  // 源健康
  addSection(Str::Sources);
  if (ns.sources.empty()) {
    add("  ", tr(Str::NoSources));
  } else {
    size_t shown = 0;
    for (const auto& s : ns.sources) {
      if (shown >= 4) {
        snprintf(buf, sizeof(buf), "+%u", static_cast<unsigned>(ns.sources.size() - shown));
        add("  ", buf);
        break;
      }
      char age[24];
      formatAge(s.last_success_at, age, sizeof(age));
      snprintf(buf, sizeof(buf), "%s · %s %s · FAIL %u",
               s.consecutive_failures == 0 ? tr(Str::Ok) : tr(Str::Fail), tr(Str::LastOk),
               age, static_cast<unsigned>(s.consecutive_failures));
      add(s.id.substr(0, 14), buf);
      shown++;
    }
  }

  // 存储统计
  if (snapshot_ != nullptr) {
    size_t active = 0, acked = 0, term = 0, muted = 0;
    for (const auto& r : snapshot_->records) {
      if (r.muted) {
        muted++;
        continue;
      }
      if (r.state == alert_core::AlertState::Active) active++;
      else if (r.state == alert_core::AlertState::Acked) acked++;
      else term++;
    }
    snprintf(buf, sizeof(buf), "%s %u · %s %u · T %u · %s %u", tr(Str::StateActive),
             static_cast<unsigned>(active), tr(Str::StateAcked), static_cast<unsigned>(acked),
             static_cast<unsigned>(term), tr(Str::MutedCount), static_cast<unsigned>(muted));
    addSection(Str::StoreStats);
    add("  ", buf);
  }

  // 系统
  addSection(Str::SysStatus);
  std::string sys;
  sys += deps_.boot.sdReady ? "SD OK " : "SD FAIL ";
  sys += deps_.boot.rtcReady ? "RTC OK " : "RTC FAIL ";
  sys += deps_.boot.touchReady ? "TP OK " : "TP FAIL ";
  sys += deps_.boot.buttonsReady ? "KEY OK" : "KEY FAIL";
  add("  ", sys);
  const int64_t up = nowMs() / 1000;
  snprintf(buf, sizeof(buf), "%llds", static_cast<long long>(up));
  add(tr(Str::Uptime), buf);
  const esp_app_desc_t* d = esp_app_get_description();
  snprintf(buf, sizeof(buf), "%s %s", d->project_name, d->version);
  add(tr(Str::Version), buf);
}

void PanelApp::buildHelp(S3Model& m) {
  auto add = [&m](Str label, Str value) {
    m.rows.emplace_back(tr(label), tr(value));
  };
  add(Str::HelpPower, Str::HelpActivateBack);
  add(Str::HelpFrontHome, Str::HelpHome);
  add(Str::HelpFrontGestures, Str::HelpDefaultHelpStatus);
  add(Str::HelpUpDown, Str::HelpFocusPage);
  add(Str::HelpSwipe, Str::HelpPage);
  add(Str::HelpChord, Str::HelpCustomOnly);
  add(Str::HelpDanger, Str::HelpFreshHold);
  add(Str::HelpCustomBindings, Str::HelpOverrideOrder);
  add(Str::HelpLocalReset, Str::HelpResetHere);
}

void PanelApp::buildS4(S4Model& m) {
  auto add = [&m](Str label, const std::string& value, bool isAction) {
    S4RowModel row;
    row.label = tr(label);
    row.value = value;
    row.isAction = isAction;
    m.rows.push_back(std::move(row));
  };
  char buf[96];
  const eego::net::NetStatusSnapshot& status = netStatus();
  add(Str::SetNetwork, status.wifi.ip.empty() ? tr(Str::WifiDisconnected) : status.wifi.ip, false);
  if (admin::ProvisionPortal::instance().apActive()) {
    snprintf(buf, sizeof(buf), "%us",
             static_cast<unsigned>(admin::ProvisionPortal::instance().windowRemainingMs() / 1000));
    add(Str::SetRecoveryAp, buf, true);
  } else {
    add(Str::SetRecoveryAp, tr(Str::Off), true);
  }
  add(Str::SetWifiReset, "", true);
  add(Str::SetBluetooth, tr(Str::Unavailable), false);
  add(Str::SetLanguage, settings_.language == "zh" ? tr(Str::LangZh) : tr(Str::LangEn), false);
  const int offset = settings_.timezone_offset_min;
  snprintf(buf, sizeof(buf), "UTC%c%02d:%02d", offset < 0 ? '-' : '+', abs(offset) / 60,
           abs(offset) % 60);
  add(Str::SetTimezone, buf, false);
  const bool frontlight = eego::FrontlightManager::instance().present();
  add(Str::SetFrontlight,
      !frontlight ? tr(Str::FrontlightMissing)
                  : (settings_.frontlight_enabled ? tr(Str::On) : tr(Str::Off)),
      false);
  snprintf(buf, sizeof(buf), "%u%%", static_cast<unsigned>(settings_.frontlight_brightness));
  add(Str::SetBrightness, frontlight ? buf : tr(Str::FrontlightMissing), false);
  snprintf(buf, sizeof(buf), "%u%%", static_cast<unsigned>(settings_.frontlight_warmth));
  add(Str::SetWarmth, frontlight ? buf : tr(Str::FrontlightMissing), false);
  if (settings_.data_refresh_s == 0) {
    add(Str::SetDataRefresh, tr(Str::SourceSchedules), false);
  } else {
    snprintf(buf, sizeof(buf), tr(Str::PollEveryFmt),
             static_cast<unsigned>(settings_.data_refresh_s));
    add(Str::SetDataRefresh, buf, false);
  }
  add(Str::SetPollNow, "", true);
  if (settings_.epd_full_refresh_s == 0) {
    add(Str::SetEpdFull, tr(Str::ManualOnly), false);
  } else {
    snprintf(buf, sizeof(buf), tr(Str::PollEveryFmt),
             static_cast<unsigned>(settings_.epd_full_refresh_s));
    add(Str::SetEpdFull, buf, false);
  }
  add(Str::SetEpdNow, "", true);
  const char* power = tr(Str::ModeAuto);
  if (settings_.power_mode == PowerMode::Mains) power = tr(Str::ModeMains);
  if (settings_.power_mode == PowerMode::Battery) power = tr(Str::ModeBattery);
  add(Str::SetPowerMode, power, false);
  snprintf(buf, sizeof(buf), tr(Str::PollEveryFmt), static_cast<unsigned>(settings_.battery_poll_s));
  add(Str::SetBatteryPoll, buf, false);
  if (settings_.idle_sleep_s == 0) {
    add(Str::SetIdleSleep, tr(Str::Off), false);
  } else {
    snprintf(buf, sizeof(buf), tr(Str::PollEveryFmt), static_cast<unsigned>(settings_.idle_sleep_s));
    add(Str::SetIdleSleep, buf, false);
  }
  if (settings_.sleep_start_min < 0) {
    add(Str::SetSleepPlan, tr(Str::NotScheduled), false);
  } else {
    snprintf(buf, sizeof(buf), "%02d:%02d-%02d:%02d", settings_.sleep_start_min / 60,
             settings_.sleep_start_min % 60, settings_.sleep_end_min / 60,
             settings_.sleep_end_min % 60);
    add(Str::SetSleepPlan, buf, false);
  }
  const uint64_t wake = nextWakeAt(nowMs());
  if (wake == 0) {
    add(Str::SetNextWake, tr(Str::NotScheduled), false);
  } else {
    formatTs(wake, buf, sizeof(buf));
    add(Str::SetNextWake, buf, false);
  }
  Str wake_reason = Str::WakeOther;
  if (wakeCause_ == ESP_SLEEP_WAKEUP_TIMER) wake_reason = Str::WakeTimer;
  else if (wakeCause_ == ESP_SLEEP_WAKEUP_EXT0) wake_reason = Str::WakeButton;
  else if (wakeCause_ == ESP_SLEEP_WAKEUP_UNDEFINED) wake_reason = Str::WakeReset;
  add(Str::SetLastWake, tr(wake_reason), false);
  add(Str::SetStorage, eego::SdCard::instance().ready() ? tr(Str::SdReady) : tr(Str::SdMissing),
      false);
  add(Str::SetDiagnostics, settings_.diagnostics_enabled ? tr(Str::On) : tr(Str::Off), false);
  add(Str::SetRunDiagnostics, settings_.diagnostics_enabled ? "" : tr(Str::Unavailable), true);
  add(Str::SetSecurity, admin::AuthManager::instance().hasPassword() ? tr(Str::SecuritySet)
                                                                     : tr(Str::SecurityMissing),
      false);
  snprintf(buf, sizeof(buf), tr(Str::MutesFmt), static_cast<unsigned>(muteRuleCount_));
  add(Str::SetMutes, buf, true);
  add(Str::SetFactoryReset, "", true);
  add(Str::Version, esp_app_get_description()->version, false);
  add(Str::SetInputHelp, "", false);
  add(Str::SetInputReset, "", true);

  m.pageCount = (m.rows.size() + kSettingsPageRows - 1) / kSettingsPageRows;
  m.page = focus_ / kSettingsPageRows;
  m.selected = focus_ % kSettingsPageRows;
  const size_t begin = m.page * kSettingsPageRows;
  const size_t end = std::min(begin + kSettingsPageRows, m.rows.size());
  std::vector<S4RowModel> visible;
  for (size_t i = begin; i < end; i++) visible.push_back(std::move(m.rows[i]));
  m.rows = std::move(visible);
}

}  // namespace panel
