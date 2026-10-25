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
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "esp_system.h"
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
constexpr uint32_t kRenderThrottleMs = 250;
constexpr int64_t kClockCheckMs = 1000;          // 状态栏分钟变化检查周期（仅分钟变化才重绘）
constexpr int64_t kBleProvRenderMs = 10000;      // BLE 配网倒计时最快 10s 重绘一次
constexpr int64_t kDoublePressSyncMs = 1000;     // 绑定修订号复查周期
constexpr uint32_t kNetIdleProbeMs = 100;        // 单次 waitIdle 探测超时（入队 + 等待各 ≤100ms）
constexpr int64_t kNetIdleFirstIntervalMs = 1000;
constexpr int64_t kNetIdleMaxIntervalMs = 5000;
// 电池定时唤醒的硬预算（自启动起）：超时即取消在途请求、渲染已有数据并深睡。
constexpr int64_t kFastWakeBudgetMs = 20000;
// 等 WiFi 的上限：到点仍未连上也发起取数批次（静态源/缓存仍可渲染）。
constexpr int64_t kFastWakeWifiWaitMs = 12000;
constexpr int64_t kInteractiveWifiWaitMs = 15000;
// 深睡前等 net_task 静默的预算（快速路径已取消在途请求，只需很短）。
constexpr int64_t kFastSleepIdleBudgetMs = 2000;
constexpr int64_t kSleepIdleBudgetMs = 8000;
// 按键唤醒：启动后这段时间内的首个按键手势是「唤醒那一下」，不执行动作。
constexpr int64_t kWakePressGuardMs = 1500;
constexpr size_t kSettingsRowsPerPage = static_cast<size_t>(geom::kSettingsRowsPerPage);
constexpr int64_t kDetailCheckMs = 1000;        // 告警详情内容变化检查周期
constexpr int64_t kOnboardingCheckMs = 2000;    // 引导页地址/WiFi 变化检查周期
constexpr int64_t kStatusRefreshMs = 30000;     // 状态页周期重绘（年龄/运行时长）


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

  mains_ = computeMains();
  NetStack::instance().wifi().setPowerSave(mains_);
  phaseStartMs_ = lastBatMs_;
  wakeStartUnix_ = TimeSource::valid() ? TimeSource::now() : 0;
  if (battery::interactiveWake(wakeSource_)) wakePressGuardUntilMs_ = lastBatMs_ + kWakePressGuardMs;
  if (!mains_) battPhase_ = fastWake_ ? BattPhase::Fetching : BattPhase::Interactive;

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
    // 快速路径不做首帧：面板保持上一帧，取数完成后由 presentSleepFrame 决定。
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
  }
  if ((changed & eego::settings::kChangedBle) != 0) {
    eego::ble::BleManager::instance().configure(settings_);
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
  // 设置变更（设备设置页或 web 端应用）视为一次用户交互。
  if (changed != 0) self->noteInteraction(self->nowMs());
}

battery::WakeInputs PanelApp::wakeInputs(uint64_t now_unix) const {
  battery::WakeInputs in;
  in.now_unix = now_unix;
  in.tz_offset_min = settings_.timezone_offset_min;
  in.interval_min = settings_.battery_refresh_min;
  in.night.start_min = settings_.sleep_start_min;
  in.night.end_min = settings_.sleep_end_min;
  // 常供电只有夜间暂停会深睡：不按电量缩放/停止唤醒。
  in.battery_known = !mains_ && batCache_.millivolts > 0;
  in.battery_pct = batCache_.percent;
  in.charging = charging_ || batCache_.charging;
  in.consecutive_failures = battery::rtcState().consecutive_failures;
  return in;
}

uint64_t PanelApp::plannedWakeAt() {
  const uint64_t now_unix = TimeSource::valid() ? TimeSource::now() : 0;
  const battery::WakePlan plan = battery::planNextWake(wakeInputs(now_unix));
  if (mains_ && plan.kind != battery::WakeKind::NightPause) return 0;
  return plan.wake_unix;
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
    noteInteraction(nowMs());  // 配网门户/恢复热点等管理事件：需立即显示
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

  if (restartRequested_) {
    // 快速路径中有人按键：以完整应用重启（RTC 状态保持；面板不清屏）。
    ESP_LOGI(TAG, "button during battery wake: restarting into interactive mode");
    NetStack::instance().wifi().powerOff();
    battery::commitRtcState();
    esp_restart();
  }

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
  if (wantMains != mains_) {
    if (fastWake_) {
      // 快速路径只初始化了最小子系统：接上电源后以完整应用重启。
      ESP_LOGI(TAG, "external power during battery wake: restarting");
      NetStack::instance().wifi().powerOff();
      battery::commitRtcState();
      esp_restart();
    }
    onModeChange(wantMains);
  }
  dashboard_.service(dashboardLive());
  if (dashboard_.consumeConfigChanged()) noteInteraction(now);  // web 端应用/删除 dashboard
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
  if (!fastWake_) eego::ble::BleManager::instance().service(mains_);
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
    // 常供电的夜间暂停：窗口内深睡到窗口结束（配网/BLE 配网期间推迟）。电池模式
    // 由唤醒计划处理（交互期超时后的深睡计划会落到窗口结束，定时唤醒同理）。
    if (battPhase_ == BattPhase::Mains && !sleepPending_ &&
        !admin::ProvisionPortal::instance().apActive() &&
        !eego::ble::BleManager::instance().sleepBlocked()) {
      uint64_t wake_at = 0;
      uint32_t remaining = 0;
      if (eego::settings::scheduledSleep(settings_, nowUnix(), wake_at, remaining)) {
        requestSleep(now, "night pause");
      }
    }
  }
  batteryService(now);
  serviceSleep(now);
  // 定时唤醒快速路径不做交互式渲染：深睡帧由 presentSleepFrame 统一决定。
  if (battPhase_ == BattPhase::Fetching) return;

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
    if (dashboard_.needsRender() && displayAllowed(now)) {
      dashboard_.render();
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

  // 状态栏时钟：仅当显示的分钟（或时间可用性）变化时重绘
  if (now - lastClockCheckMs_ >= kClockCheckMs) {
    lastClockCheckMs_ = now;
    const int64_t key =
        TimeSource::valid() ? static_cast<int64_t>(TimeSource::now() / 60) : -1;
    if (key != renderedClockKey_) markDirty();
  }

  if (dirty_ && now - lastRenderMs_ >= kRenderThrottleMs && displayAllowed(now)) {
    render(now);
    displayedThisBoot_ = true;
    noteDisplayed();
  }
}

// ── 空闲感知刷新节奏 ────────────────────────────────────────────────────────

void PanelApp::configureRefreshPolicy() {
  RefreshPolicyConfig config;
  // 电池模式：唤醒 → 渲染 → 深睡，显示节奏由电源 FSM 决定，不做空闲合并。
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
  // 配网（门户页 / BLE 配网倒计时）期间用户正在操作手机：始终即时刷新。
  if (page_ == Page::Provision || eego::ble::BleManager::instance().provisioningActive()) {
    return true;
  }
  if (!refreshPolicy_.idle(now)) return refreshPolicy_.allow(now, 0, false);
  if (!idleLogged_) {
    idleLogged_ = true;
    ESP_LOGI(TAG, "display: active -> idle (refresh every %u min on the boundary)",
             static_cast<unsigned>(settings_.idle_refresh_min));
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
  if (settings_.idle_critical_immediate) {
    refreshPolicy_.noteRendered(unix_now, dashboard_.activeCriticalIdentities());
  } else {
    refreshPolicy_.noteRendered(unix_now);
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
  if (fastWake_) {
    restartRequested_ = true;  // 快速路径没有完整输入/显示栈：重启进交互模式
    return;
  }
  if (ev.action == eego::ButtonAction::Press && wakePressGuardUntilMs_ != 0 &&
      now_us / 1000 < wakePressGuardUntilMs_) {
    // 按键唤醒：唤醒的那一下（ButtonManager 启动时仍按着）只唤醒，不执行动作。
    wakePressGuardUntilMs_ = 0;
    buttonGestureStale_[index] = true;
    noteInteraction(now_us / 1000);
    return;
  }
  lastInputMs_ = now_us / 1000;
  noteInteraction(lastInputMs_);
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
  noteInteraction(lastInputMs_);
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
constexpr uint32_t kBatteryInteractiveSteps[] = {20, 30, 60, 120, 300, 600};  // 秒
constexpr uint32_t kIdleAfterSteps[] = {0, 1, 2, 5, 10, 15, 30, 60};  // 分钟；0 = 关闭
constexpr uint32_t kIdleRefreshSteps[] = {2, 5, 10, 15, 30, 60};       // 分钟（整除 60）
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
    case SettingId::IdleAfter:
    case SettingId::IdleRefresh:
    case SettingId::PowerMode:
    case SettingId::BatteryRefresh:
    case SettingId::BatteryMaxStale:
    case SettingId::BatteryInteractive:
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
    case SettingId::IdleAfter:
      next.idle_after_min = stepChoice(kIdleAfterSteps, settings_.idle_after_min, step, wrap);
      break;
    case SettingId::IdleRefresh:
      next.idle_refresh_min =
          stepChoice(kIdleRefreshSteps, settings_.idle_refresh_min, step, wrap);
      break;
    case SettingId::PowerMode: {
      constexpr PowerMode kModes[] = {PowerMode::Auto, PowerMode::Mains, PowerMode::Battery};
      next.power_mode = stepChoice(kModes, settings_.power_mode, step, true);
      break;
    }
    case SettingId::BatteryRefresh:
      next.battery_refresh_min = stepChoice(eego::settings::kBatteryRefreshChoices,
                                            settings_.battery_refresh_min, step, wrap);
      // 最长不刷屏不得短于唤醒间隔：随之抬到第一个合法档。
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
  noteInteraction(nowMs());  // 电池交互期：恢复热点开启期间不会深睡
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
  noteInteraction(nowMs());
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
  return battery::mainsPowered(static_cast<uint8_t>(settings_.power_mode),
                               charging_ || batCache_.charging, batCache_.millivolts);
}

void PanelApp::onModeChange(bool mains) {
  mains_ = mains;
  configureRefreshPolicy();          // 电池模式不做空闲合并
  noteInteraction(nowMs());          // 插拔电源视为一次交互
  ESP_LOGI(TAG, "power mode -> %s", mains ? "mains" : "battery");
  NetStack::instance().wifi().setPowerSave(mains);  // 常供电 modem-sleep
  eego::ble::BleManager::instance().onPowerMode(mains);
  eego::ble::BleManager::instance().setBatteryScanWindow(false);
  if (mains) {
    battPhase_ = BattPhase::Mains;
    // 常供电不再走电池深睡（夜间暂停仍由 service() 的窗口检查发起）。
    sleepPending_ = false;
    sleepIdleWait_.active = false;
    NetStack::instance().reconnectNow();
  } else {
    enterInteractive(nowMs());  // 拔电时用户就在旁边：先保持交互，超时后对齐深睡
  }
  dashboard_.setMains(dashboardLive());
  markDirty(true);
}

void PanelApp::enterInteractive(int64_t now) {
  battPhase_ = BattPhase::Interactive;
  phaseStartMs_ = now;
  batchStarted_ = false;
  noteInteraction(now);
  dashboard_.setBatteryStatus("");
  // BLE 被动扫描：电池模式每个交互窗口最多扫一轮（与旧 Serving 窗口一致）。
  eego::ble::BleManager::instance().setBatteryScanWindow(true);
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

void PanelApp::batteryService(int64_t now) {
  if (sleepPending_) return;
  const bool wifi_up = wifiState_.load() == 2 /*Connected*/;
  if (wifi_up) wifiSeen_ = true;
  switch (battPhase_) {
    case BattPhase::Mains:
      return;
    case BattPhase::Fetching: {
      // 定时唤醒：一批拉取全部到期源，硬预算 kFastWakeBudgetMs（自启动起），到点
      // 无论结果如何都渲染已有数据并深睡。
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
      // 配网门户/恢复热点/BLE 配网期间不睡（用户正在用手机操作）。
      if (admin::ProvisionPortal::instance().apActive() ||
          eego::ble::BleManager::instance().sleepBlocked()) {
        noteInteraction(now);
        return;
      }
      if (!batchStarted_ && (wifi_up || now - phaseStartMs_ >= kInteractiveWifiWaitMs)) {
        // 交互期 dashboard 按常供电节奏自行拉取；这里只记录批次起点用于失败计数。
        batchStarted_ = true;
      }
      // 管理后台的已认证请求同样算「有人在用」（浏览 web 端时不深睡）。
      const int64_t last = std::max(refreshPolicy_.lastInteractionMs(),
                                    admin::lastAuthenticatedRequestMs());
      if (now - last >= static_cast<int64_t>(settings_.battery_interactive_timeout_s) * 1000) {
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
  // 停止取数：在途请求被取消，后续 tick 不再发起新请求（深睡前不带 HTTP 断电）。
  dashboard_.cancelBatteryCycle();
  startNetIdleWait(sleepIdleWait_, now, fastWake_ ? kFastSleepIdleBudgetMs : kSleepIdleBudgetMs);
}

void PanelApp::serviceSleep(int64_t now) {
  if (!sleepPending_) return;
  // 交互期内有新输入/管理请求 → 取消本次休眠（常供电夜间暂停/定时唤醒不受影响）。
  if (battPhase_ == BattPhase::Interactive &&
      std::max(refreshPolicy_.lastInteractionMs(), admin::lastAuthenticatedRequestMs()) >
          sleepRequestedMs_) {
    ESP_LOGI(TAG, "sleep cancelled by input");
    sleepPending_ = false;
    sleepIdleWait_.active = false;
    dashboard_.setMains(dashboardLive());  // 恢复取数
    return;
  }
  if (admin::ProvisionPortal::instance().apActive() ||
      eego::ble::BleManager::instance().provisioningActive()) {
    ESP_LOGW(TAG, "recovery/provisioning active, sleep postponed");
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

  // 失败退避：只有电池取数批次计数（夜间暂停/常供电不计）。
  if (!mains_ && batchStarted_) {
    if (batchSucceeded()) {
      rtc.consecutive_failures = 0;
      rtc.last_data_unix = now_unix;
    } else if (rtc.consecutive_failures < 16) {
      ++rtc.consecutive_failures;
    }
  }
  const battery::WakePlan plan = battery::planNextWake(wakeInputs(now_unix));

  // 无线先关（刷屏的数秒内 WiFi 不再耗电）；记录本次关联的 AP 供下次快连。
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
    rtc.ap_valid = 0;  // 提示没能连上：下次走常规扫描
  }
  NetStack::instance().wifi().powerOff();

  presentSleepFrame(plan, now_unix);

  rtc.last_wake_unix = now_unix;
  ++rtc.wakes;
  battery::commitRtcState();
  if (deps_.boot.displayReady && deps_.display != nullptr) deps_.display->deepSleep();

  ESP_LOGI(TAG, "deep sleep: kind=%d interval=%umin wake=%llu failures=%u",
           static_cast<int>(plan.kind), static_cast<unsigned>(plan.interval_min),
           static_cast<unsigned long long>(plan.wake_unix),
           static_cast<unsigned>(rtc.consecutive_failures));
  // 电量过低：只留电源键（↑/↓ 误触会反复耗电）；其余情况电源键 + ↑/↓。
  battery::deepSleep(plan.sleep_s, plan.kind != battery::WakeKind::CriticalBattery);
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

std::string PanelApp::batteryStatusText(uint8_t bucket, uint64_t stamp,
                                        uint32_t interval_min) const {
  char buf[96];
  if (stamp < battery::kMinValidUnix) {
    snprintf(buf, sizeof(buf), tr(Str::BatStatusNoClockFmt), static_cast<unsigned>(bucket));
    return buf;
  }
  battery::NightWindow night{settings_.sleep_start_min, settings_.sleep_end_min};
  const uint64_t deadline =
      battery::refreshDeadline(stamp, settings_.battery_max_stale_min,
                               settings_.timezone_offset_min, interval_min, night);
  int uh = 0;
  int um = 0;
  int dh = 0;
  int dm = 0;
  battery::localHourMinute(stamp, settings_.timezone_offset_min, uh, um);
  battery::localHourMinute(deadline, settings_.timezone_offset_min, dh, dm);
  snprintf(buf, sizeof(buf), tr(Str::BatStatusFmt), static_cast<unsigned>(bucket), uh, um, dh, dm);
  return buf;
}

std::string PanelApp::nightPauseText(uint64_t resume_unix) const {
  char buf[64];
  int h = 0;
  int m = 0;
  battery::localHourMinute(resume_unix, settings_.timezone_offset_min, h, m);
  snprintf(buf, sizeof(buf), tr(Str::NightPauseFmt), h, m);
  return buf;
}

void PanelApp::drawNotice(const char* title, const char* line1, const char* line2,
                          const char* line3) {
  eego::PortraitCanvas& c = *deps_.canvas;
  c.clear();
  c.useUiContentRect();
  // 标题 + 三行说明，整体在内容区垂直居中，左对齐于 kInnerX。
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

void PanelApp::presentSleepFrame(const battery::WakePlan& plan, uint64_t now_unix) {
  if (!deps_.boot.displayReady || deps_.canvas == nullptr || deps_.display == nullptr) return;
  battery::RtcState& rtc = battery::rtcState();
  battery::FrameRecord& rec = rtc.frame;
  const bool critical = plan.kind == battery::WakeKind::CriticalBattery;
  const bool night = plan.kind == battery::WakeKind::NightPause;
  // 面板内容是否就是 rec 记录的帧：只有快速路径能保证（交互期刷过别的画面）。
  const bool panel_known = fastWake_ && rec.valid != 0;
  const uint32_t interval = plan.interval_min != 0 ? plan.interval_min : settings_.battery_refresh_min;
  const uint8_t bucket = battery::batteryDisplayBucket(batCache_.percent, rtc.battery_bucket);
  // 「更新于」：定时唤醒记为名义边界（吸收提前醒），其余记为当前分钟。
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
      // 交互期画面任意：回到 dashboard 主页并整帧重画。
      page_ = Page::Dashboard;
      dashboard_.home();
      deps_.canvas->clear();
      dashboard_.invalidateAll();
    }
    if (night) {
      dashboard_.setBatteryStatus(nightPauseText(plan.wake_unix));
      dashboard_.renderOffscreen();
      hash = frameHash();
      skip = panel_known && hash == rec.hash;
    } else if (mains_) {
      dashboard_.setBatteryStatus("");
      dashboard_.renderOffscreen();
      hash = frameHash();
    } else {
      // 先按上次的「更新于」渲染：数据不变 ⇒ 帧完全相同 ⇒ 不刷屏（时钟不会逼出刷新）。
      if (panel_known) {
        dashboard_.setBatteryStatus(batteryStatusText(bucket, rec.refresh_unix, interval));
        dashboard_.renderOffscreen();
        hash = frameHash();
        skip = hash == rec.hash && !battery::frameStale(rec, now_unix, settings_.battery_max_stale_min);
      }
      if (!skip) {
        dashboard_.setBatteryStatus(batteryStatusText(bucket, slot, interval));
        dashboard_.renderOffscreen();
        hash = frameHash();
      }
    }
  } else {
    // 未配置 dashboard：引导页上的地址深睡期间不可用，换成说明页。
    drawNotice(tr(Str::Sleeping), tr(Str::WakeHint), "", tr(Str::DeepSleepOffline));
    hash = frameHash();
    skip = panel_known && hash == rec.hash;
  }
  if (skip) {
    ESP_LOGI(TAG, "sleep frame unchanged: panel untouched");
    return;
  }

  // 刷新方式：快速路径需把 SD 上的上一帧预载进 DTM1 才能 Fast；交互期控制器
  // 本就持有当前面板内容（本次启动刷过屏）。
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
      case SettingId::PowerMode:
        label = Str::SetPowerMode;
        value = tr(settings_.power_mode == PowerMode::Mains     ? Str::ModeMains
                   : settings_.power_mode == PowerMode::Battery ? Str::ModeBattery
                                                                : Str::ModeAuto);
        break;
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
