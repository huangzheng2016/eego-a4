// SPDX-License-Identifier: GPL-3.0-or-later
// 告警面板应用状态机实现（契约见 panel_app.h）。
#include "panel_app.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
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

// net 事件位（notifyNetEvent 原子置位，drainNetBits 消费）
constexpr uint32_t kNetBitWifi = 1u << 0;
constexpr uint32_t kNetBitSource = 1u << 1;

// 电源/电池常量
constexpr uint32_t kAwakeWindowMs = 90000;   // 电池模式交互窗口（输入可延长）
constexpr uint32_t kPollWaitMs = 45000;      // 电池模式等 WiFi 上限
constexpr uint32_t kRenderThrottleMs = 250;  // 渲染最小间隔（e-ink 节奏）
constexpr uint32_t kClockRenderMs = 30000;   // 状态栏时钟节奏
constexpr uint32_t kConfirmWindowMs = 5000;  // 二次确认窗口

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
  prefs_.load();
  setLang(prefs_.lang());
  adminQueue_ = xQueueCreate(8, sizeof(AdminMail));

  charging_ = eego::PowerManager::instance().isCharging();
  batCache_ = eego::PowerManager::instance().readBattery();
  lastBatMs_ = nowMs();
  lastChargeMs_ = lastBatMs_;
  lastInputMs_ = lastBatMs_;

  mains_ = computeMains();
  NetStack::instance().wifi().setPowerSave(mains_);  // 常供电 = modem-sleep
  if (!mains_) {
    battPhase_ = BattPhase::Polling;
    cycleStartMs_ = lastBatMs_;
    pollDeadlineMs_ = cycleStartMs_ + kPollWaitMs;
  }

  // 前光按 admin 配置应用（缺席时配置被忽略，S4 显示"无前光"）
  const admin::AdminDomainConfig ac = admin::ConfigStore::instance().admin();
  if (eego::FrontlightManager::instance().present() && ac.brightness > 0) {
    eego::FrontlightManager::instance().setWarm(
        static_cast<uint8_t>(ac.warmth < 0 ? 0 : (ac.warmth > 100 ? 100 : ac.warmth)));
    eego::FrontlightManager::instance().setBrightness(
        static_cast<uint8_t>(ac.brightness > 100 ? 100 : ac.brightness));
    frontlightOn_ = true;
  }

  ESP_LOGI(TAG, "panel begin: mains=%d lang=%s bat=%umV/%u%%", mains_ ? 1 : 0,
           langCode(prefs_.lang()), batCache_.millivolts, batCache_.percent);
  markDirty(true);
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

  if (confirm_ != ConfirmKind::None && now > confirmDeadlineMs_) clearConfirm();
  if (!banner_.empty() && now > bannerDeadlineMs_) {
    banner_.clear();
    markDirty();
  }

  // 详情目标消失（过期/淘汰）→ 退回列表
  if (page_ == Page::Detail && findRecord(detailId_) == nullptr) {
    back();
  }

  // 模式切换（充电插拔/设置变更）
  const bool wantMains = computeMains();
  if (wantMains != mains_) onModeChange(wantMains);

  batteryFsm(now);

  // 状态栏时钟节奏（页面停留时也保持时间新鲜）
  if (now - lastRenderMs_ > kClockRenderMs) markDirty();

  if (dirty_ && now - lastRenderMs_ >= kRenderThrottleMs) {
    render(now);
  }
}

// ── 输入 ────────────────────────────────────────────────────────────────────

void PanelApp::handleTouch(const eego::TouchEvent& ev) {
  lastInputMs_ = nowMs();
  switch (ev.type) {
    case eego::TouchEventType::Touch:
      if (ev.pressed) onTouchTap(static_cast<int16_t>(ev.x), static_cast<int16_t>(ev.y));
      break;
    case eego::TouchEventType::FrontKeyShort:
      clearConfirm();
      onFrontKeyShort();
      break;
    case eego::TouchEventType::FrontKeyLong:
      onFrontKeyLong();
      break;
    default:
      break;
  }
}

void PanelApp::handleButton(const eego::ButtonEvent& ev) {
  lastInputMs_ = nowMs();
  if (ev.action == eego::ButtonAction::LongPress) {
    onButtonLong(ev.id);
  } else {
    onButtonShort(ev.id);
  }
}

void PanelApp::onButtonShort(eego::ButtonId id) {
  // 二次确认执行：armed 状态下电源短按 = 确认（其他输入在各自分支先取消）
  if (id == eego::ButtonId::Power && confirm_ != ConfirmKind::None) {
    executeArmedConfirm();
    return;
  }

  clearConfirm();
  switch (id) {
    case eego::ButtonId::Up:
      moveFocus(-1);
      break;
    case eego::ButtonId::Down:
      moveFocus(1);
      break;
    case eego::ButtonId::Power:
      activateFocused();
      break;
  }
}

void PanelApp::onButtonLong(eego::ButtonId id) {
  if (id == eego::ButtonId::Power) {
    clearConfirm();
    back();
  } else {
    onButtonShort(id);  // Up/Down 长按视同短按
  }
}

void PanelApp::onFrontKeyShort() {
  // S0 ↔ S1 快切；其他页回 S0
  if (page_ == Page::Overview) {
    gotoPage(Page::Table);
  } else {
    gotoPage(Page::Overview);
  }
}

void PanelApp::onFrontKeyLong() {
  // 任意页面：全部确认（二次确认）
  if (confirm_ == ConfirmKind::AckAll) {
    doAckAll();
    clearConfirm();
  } else {
    armConfirm(ConfirmKind::AckAll);
  }
}

// ── 导航 ────────────────────────────────────────────────────────────────────

void PanelApp::gotoPage(Page p) {
  if (page_ == p) return;
  page_ = p;
  focus_ = 0;
  provisionSucceeded_ = false;
  clearConfirm();
  if (p == Page::Settings) refreshMuteRuleCount();  // S4 静音规则数（M21）
  markDirty(true);  // 切页 Full 刷新（避免跨页残影）
}

void PanelApp::back() {
  switch (page_) {
    case Page::Detail: {
      // 返回列表时把选择还原到目标行（gotoPage 会清 focus，先切页再还原）
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
    case Page::Table:
    case Page::Status:
    case Page::Settings:
      gotoPage(Page::Overview);
      break;
    default:
      break;
  }
}

void PanelApp::moveFocus(int delta) {
  switch (page_) {
    case Page::Overview:
      focus_ = (focus_ + delta + 3) % 3;
      markDirty();
      break;
    case Page::Table: {
      std::vector<const alert_core::AlertRecord*> vis;
      const size_t total = visibleRecords(vis);
      if (total == 0) break;
      int64_t idx = static_cast<int64_t>(tablePage_ * geom::kRowsPerPage + focus_) + delta;
      if (idx < 0) idx = 0;
      if (idx >= static_cast<int64_t>(total)) idx = static_cast<int64_t>(total) - 1;
      tablePage_ = static_cast<size_t>(idx) / geom::kRowsPerPage;
      focus_ = static_cast<size_t>(idx) % geom::kRowsPerPage;
      markDirty();
      break;
    }
    case Page::Detail:
      focus_ = (focus_ + delta + 4) % 4;
      markDirty();
      break;
    case Page::Status:
      focus_ = (focus_ + delta + 2) % 2;
      markDirty();
      break;
    case Page::Settings: {
      const size_t rows = 8;
      focus_ = (focus_ + delta + rows) % rows;
      markDirty();
      break;
    }
    default:
      break;
  }
}

void PanelApp::activateFocused() {
  switch (page_) {
    case Page::Overview:
    case Page::Detail:
    case Page::Status:
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
      activateSoftKey(focus_ + 1);  // 软键 0 = 返回；行聚焦从 1 起
      break;
    default:
      break;
  }
}

void PanelApp::activateSoftKey(size_t idx) {
  switch (page_) {
    case Page::Overview:
      if (idx == 0) {
        gotoPage(Page::Table);
      } else if (idx == 1) {
        if (confirm_ == ConfirmKind::AckAll) {
          doAckAll();
          clearConfirm();
        } else {
          armConfirm(ConfirmKind::AckAll);
        }
      } else if (idx == 2) {
        gotoPage(Page::Settings);
      }
      break;
    case Page::Table:
      if (idx == 0) {
        activateFocused();
      } else if (idx == 1) {
        back();
      }
      break;
    case Page::Detail: {
      const alert_core::AlertRecord* rec = findRecord(detailId_);
      if (idx == 0 && rec != nullptr && rec->state == alert_core::AlertState::Active) {
        doAck(detailId_);
      } else if (idx == 1 && rec != nullptr) {
        if (confirm_ == ConfirmKind::Mute) {
          doMute(*rec);
          clearConfirm();
        } else {
          armConfirm(ConfirmKind::Mute);
        }
      } else if (idx == 2 && rec != nullptr &&
                 (rec->state == alert_core::AlertState::Active ||
                  rec->state == alert_core::AlertState::Acked)) {
        if (confirm_ == ConfirmKind::Clear) {
          doClear(detailId_);
          clearConfirm();
        } else {
          armConfirm(ConfirmKind::Clear);
        }
      } else if (idx == 3) {
        back();
      }
      break;
    }
    case Page::Status:
      if (idx == 0) {
        doPollNow();
      } else if (idx == 1) {
        back();
      }
      break;
    case Page::Settings:
      if (idx == 0) {
        back();
        break;
      }
      activateSettingRow(idx - 1);
      break;
    default:
      break;
  }
}

// ── 触控 ────────────────────────────────────────────────────────────────────

void PanelApp::onTouchTap(int16_t x, int16_t y) {
  // 1. 命中解析：软键下标（S4 软键 0=返回）或 S4 行（hitIdx=行下标+1, hitRow=true）；
  //    内容区命中保持 hitIdx=-1（永不匹配布防 → 取消）。
  int hitIdx = -1;
  bool hitRow = false;
  if (y >= geom::kSoftKeyY && y <= geom::kSoftKeyY + geom::kSoftKeyH) {
    const size_t n = softKeyCount();
    if (n > 0) {
      const int16_t gap = 8;
      const int16_t kw = static_cast<int16_t>((geom::kContentW - gap * (n - 1)) / n);
      int idx = (x - geom::kContentX) / (kw + gap);
      if (idx < 0) idx = 0;
      if (idx >= static_cast<int>(n)) idx = static_cast<int>(n) - 1;
      hitIdx = idx;
    }
  } else if (page_ == Page::Settings) {
    const int16_t rowsY = geom::kContentTop + 64;
    if (y >= rowsY && y < rowsY + 8 * 56) {
      hitRow = true;
      hitIdx = static_cast<int>((y - rowsY) / 56) + 1;
    }
  }

  // 2. 二次确认：布防状态下命中同一控件 = 执行；过期或其他命中 = 取消后正常分发
  //    （与 onButtonShort 的语义一致，触控/按键同契约）。
  if (confirm_ != ConfirmKind::None) {
    if (nowMs() > confirmDeadlineMs_) {
      clearConfirm();
    } else if (confirmTargetMatches(confirm_, page_, hitIdx, hitRow)) {
      executeArmedConfirm();
      return;
    } else {
      clearConfirm();
    }
  }

  // 3. 正常分发
  if (hitIdx >= 0 && !hitRow) {  // 软键栏
    if (page_ == Page::Settings) {
      back();  // S4 软键只有"返回"
    } else {
      activateSoftKey(static_cast<size_t>(hitIdx));
    }
    return;
  }
  if (hitRow) {  // S4 设置行
    const size_t row = static_cast<size_t>(hitIdx - 1);
    if (row == focus_) {
      activateSoftKey(static_cast<size_t>(hitIdx));
    } else {
      focus_ = row;
      markDirty();
    }
    return;
  }
  // 页面内容
  switch (page_) {
    case Page::Overview:
      if (y >= geom::kContentTop + 64 && y < geom::kContentTop + 474) {
        gotoPage(Page::Table);  // 卡片区域点按进列表
      }
      break;
    case Page::Table: {
      const int16_t rowsY = geom::kContentTop + 60;
      if (y >= rowsY && y < rowsY + geom::kRowsPerPage * geom::kRowH) {
        const size_t row = static_cast<size_t>((y - rowsY) / geom::kRowH);
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
  switch (row) {
    case 0: {  // 语言
      const Lang next = (prefs_.lang() == Lang::Zh) ? Lang::En : Lang::Zh;
      prefs_.setLang(next);
      prefs_.save();
      setLang(next);
      markDirty(true);
      break;
    }
    case 1: {  // 电源模式
      PowerMode next;
      switch (prefs_.powerMode()) {
        case PowerMode::Auto: next = PowerMode::Mains; break;
        case PowerMode::Mains: next = PowerMode::Battery; break;
        default: next = PowerMode::Auto; break;
      }
      prefs_.setPowerMode(next);
      prefs_.save();
      markDirty();
      break;
    }
    case 2: {  // 电池轮询周期
      uint32_t next;
      switch (prefs_.batteryPollS()) {
        case 300: next = 900; break;
        case 900: next = 1800; break;
        case 1800: next = 3600; break;
        default: next = 300; break;
      }
      prefs_.setBatteryPollS(next);
      prefs_.save();
      markDirty();
      break;
    }
    case 3: {  // 前光开关
      eego::FrontlightManager& fl = eego::FrontlightManager::instance();
      if (!fl.present()) break;
      frontlightOn_ = !frontlightOn_;
      const admin::AdminDomainConfig ac = admin::ConfigStore::instance().admin();
      fl.setBrightness(frontlightOn_
                           ? static_cast<uint8_t>(ac.brightness > 0 ? ac.brightness : 60)
                           : 0);
      markDirty();
      break;
    }
    case 4:  // 静音列表（清除需二次确认；store 级规则清空，M21）
      if (confirm_ == ConfirmKind::ClearMutes) {
        doClearMutes();
        clearConfirm();
        markDirty();
      } else {
        armConfirm(ConfirmKind::ClearMutes);
      }
      break;
    case 5:  // 立即轮询
      doPollNow();
      break;
    case 6: {  // 管理后台地址
      const eego::net::NetStatusSnapshot& ns = netStatus();
      if (!ns.wifi.ip.empty()) {
        showBannerFmt(5000, "http://%s/", ns.wifi.ip.c_str());
      } else {
        showBanner(tr(Str::WifiDisconnected), 3000);
      }
      break;
    }
    case 7: {  // 版本
      const esp_app_desc_t* d = esp_app_get_description();
      showBannerFmt(5000, "%s %s", d->project_name, d->version);
      break;
    }
    default:
      break;
  }
}

void PanelApp::executeArmedConfirm() {
  switch (confirm_) {
    case ConfirmKind::AckAll:
      doAckAll();
      break;
    case ConfirmKind::Mute: {
      const alert_core::AlertRecord* rec = findRecord(detailId_);
      if (rec != nullptr) {
        doMute(*rec);
      } else {
        showBanner(tr(Str::ActionFailed), 3000);
      }
      break;
    }
    case ConfirmKind::Clear:
      doClear(detailId_);
      break;
    case ConfirmKind::ClearMutes:
      doClearMutes();
      break;
    default:
      break;
  }
  clearConfirm();
}

void PanelApp::armConfirm(ConfirmKind kind) {
  confirm_ = kind;
  confirmDeadlineMs_ = nowMs() + kConfirmWindowMs;
  if (kind == ConfirmKind::AckAll) {
    showBanner(tr(Str::ConfirmAckAll), kConfirmWindowMs);
  } else {
    showBanner(tr(Str::ConfirmHint), kConfirmWindowMs);
  }
}

void PanelApp::clearConfirm() {
  if (confirm_ == ConfirmKind::None) return;
  confirm_ = ConfirmKind::None;
  if (banner_ == tr(Str::ConfirmHint) || banner_ == tr(Str::ConfirmAckAll)) {
    banner_.clear();
  }
  markDirty();
}

void PanelApp::showBanner(const char* text, uint32_t ms) {
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
  switch (prefs_.powerMode()) {
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
  ESP_LOGI(TAG, "power mode -> %s", mains ? "mains" : "battery");
  NetStack::instance().wifi().setPowerSave(mains);  // 常供电 modem-sleep
  if (mains) {
    battPhase_ = BattPhase::Mains;
    NetStack::instance().reconnectNow();
  } else {
    battPhase_ = BattPhase::Polling;
    cycleStartMs_ = nowMs();
    pollDeadlineMs_ = cycleStartMs_ + kPollWaitMs;
    showBanner(tr(Str::Polling), 5000);
    NetStack::instance().reconnectNow();
  }
  markDirty(true);
}

void PanelApp::batteryFsm(int64_t now) {
  switch (battPhase_) {
    case BattPhase::Mains:
      return;
    case BattPhase::Polling:
      // 等 WiFi 就绪或超时 → 全源轮询 → 静默 → 进入交互窗口
      if (wifiState_.load() == 2 /*Connected*/ || now > pollDeadlineMs_) {
        ESP_LOGI(TAG, "battery poll cycle: poll all sources");
        NetStack::instance().pollNow("");
        // 显式处理静默等待超时：记录并重试一轮；仍超时只影响本轮数据新鲜度，
        // 下周期重试（不在此断电任何硬件）
        if (!NetStack::instance().waitIdle(30000)) {
          ESP_LOGW(TAG, "battery poll: waitIdle timeout, retrying once");
          if (!NetStack::instance().waitIdle(10000)) {
            ESP_LOGW(TAG, "battery poll: net still not idle, serving with current data");
          }
        }
        battPhase_ = BattPhase::Serving;
        awakeDeadlineMs_ = now + kAwakeWindowMs;
        markDirty(true);
      }
      return;
    case BattPhase::Serving: {
      // 交互窗口：近期输入延长 90s，但累计交互不超过周期起点后 10 分钟
      const int64_t hardCapMs = cycleStartMs_ + 600000;
      if (now - lastInputMs_ < 15000 && now < hardCapMs) {
        awakeDeadlineMs_ = now + kAwakeWindowMs;
        if (awakeDeadlineMs_ > hardCapMs) awakeDeadlineMs_ = hardCapMs;
      }
      if (now > awakeDeadlineMs_) enterSleep(now);
      return;
    }
  }
}

void PanelApp::enterSleep(int64_t now) {
  ESP_LOGI(TAG, "battery: entering deep sleep");
  // 实际睡眠时长 = 轮询周期剩余（不是完整周期），先算好供休眠画面与唤醒定时共用
  int64_t sleepMs = cycleStartMs_ + static_cast<int64_t>(prefs_.batteryPollS()) * 1000 - now;
  if (sleepMs < 10000) sleepMs = 10000;  // 防高频醒睡循环

  // 1. 网络静默（显式处理超时：重试一轮；仍超时则中止本次休眠回 Serving，
  //    30s 后重试——绝不带在途 HTTP 传输断电 WiFi）
  if (!NetStack::instance().waitIdle(10000) &&
      !NetStack::instance().waitIdle(5000)) {
    ESP_LOGW(TAG, "battery: net not idle after retries, sleep aborted this round");
    battPhase_ = BattPhase::Serving;
    awakeDeadlineMs_ = now + 30000;
    markDirty();
    return;
  }
  NetStack::instance().wifi().powerOff();

  // 2. 休眠画面（Full 刷新；e-ink 断电保持图像；唤醒时刻 = now + 实际睡眠时长）
  if (deps_.boot.displayReady && deps_.canvas != nullptr) {
    eego::PortraitCanvas& c = *deps_.canvas;
    c.clear();
    c.useUiContentRect();
    eego::FontManager::instance().drawTextGray(c, 76, 320, tr(Str::Sleeping), 2, 0);
    eego::FontManager::instance().drawTextGray(c, 76, 360, tr(Str::WakeHint), 1, 6);
    char wake[48];
    const uint64_t wakeTs = nowUnix() + static_cast<uint64_t>(sleepMs / 1000);
    struct tm tm_buf;
    time_t t = static_cast<time_t>(wakeTs);
    gmtime_r(&t, &tm_buf);
    snprintf(wake, sizeof(wake), "%02d-%02d %02d:%02d", tm_buf.tm_mon + 1, tm_buf.tm_mday,
             tm_buf.tm_hour, tm_buf.tm_min);
    eego::FontManager::instance().drawTextGray(c, 76, 396, wake, 1, 6);
    c.display(eego::RefreshMode::Full);
    c.waitRefreshComplete();
    if (deps_.display != nullptr) deps_.display->deepSleep();
  }

  // 3. GPIO4 锁存保持（深睡不断电；boot 已 hold，这里再确认）
  gpio_hold_en(static_cast<gpio_num_t>(eego::POWER_LATCH));
  gpio_deep_sleep_hold_en();

  // 4. 唤醒源：定时（实际睡眠时长）+ 电源键 EXT0（GPIO8 active-high）
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
  time_t t = static_cast<time_t>(ts);
  gmtime_r(&t, &tm_buf);
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
    case Page::Overview: return 3;
    case Page::Table: return 2;
    case Page::Detail: return 4;
    case Page::Status: return 2;
    case Page::Settings: return 1;
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
      out[0] = {tr(Str::Ack), active};
      out[1] = {tr(Str::Mute), rec != nullptr && !muted};
      out[2] = {tr(Str::Clear), clearable};
      out[3] = {tr(Str::Back), true};
      break;
    }
    case Page::Status:
      out[0] = {tr(Str::Refresh), true};
      out[1] = {tr(Str::Back), true};
      break;
    case Page::Settings:
      out[0] = {tr(Str::Back), true};
      break;
    default:
      break;
  }
}

// ── 渲染 ────────────────────────────────────────────────────────────────────

void PanelApp::render(int64_t now) {
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
    case Page::Provision: {
      ProvisionModel m;
      m.succeeded = provisionSucceeded_;
      snprintf(m.windowText, sizeof(m.windowText), tr(Str::ProvWindowFmt),
               static_cast<unsigned>(admin::ProvisionPortal::instance().windowRemainingMs() /
                                     1000));
      drawProvision(c, m);
      break;
    }
  }

  SoftKeyModel keys[4];
  const size_t n = softKeyCount();
  softKeys(keys, 4);
  size_t focusIdx = static_cast<size_t>(-1);
  if (page_ == Page::Overview || page_ == Page::Detail || page_ == Page::Status) {
    focusIdx = focus_;
  }
  drawSoftKeys(c, keys, n, focusIdx);

  c.display(forceFull_ ? eego::RefreshMode::Full : eego::RefreshMode::Fast);
  c.waitRefreshComplete();
  forceFull_ = false;
  dirty_ = false;
  lastRenderMs_ = now;
}

void PanelApp::buildStatusBar(StatusBarModel& m) {
  // 统一时间源（M21）：可信显示 MM-DD HH:MM；不可信保持 "--:--"（与未知时间
  // 的既有约定一致；明确的「时间不可用」在 S3 状态页行内显示）
  if (TimeSource::valid()) {
    const uint64_t now = TimeSource::now();
    struct tm tm_buf;
    time_t t = static_cast<time_t>(now);
    gmtime_r(&t, &tm_buf);
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
  if (prefs_.powerMode() == PowerMode::Mains) pmode = tr(Str::ModeMains);
  if (prefs_.powerMode() == PowerMode::Battery) pmode = tr(Str::ModeBattery);
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

void PanelApp::buildS4(S4Model& m) {
  m.selected = focus_;
  auto add = [&m](Str label, const std::string& value, bool isAction) {
    S4RowModel r;
    r.label = tr(label);
    r.value = value;
    r.isAction = isAction;
    m.rows.push_back(std::move(r));
  };
  add(Str::SetLanguage, prefs_.lang() == Lang::Zh ? tr(Str::LangZh) : tr(Str::LangEn), false);
  const char* pmode = tr(Str::ModeAuto);
  if (prefs_.powerMode() == PowerMode::Mains) pmode = tr(Str::ModeMains);
  if (prefs_.powerMode() == PowerMode::Battery) pmode = tr(Str::ModeBattery);
  add(Str::SetPowerMode, pmode, false);
  char buf[48];
  snprintf(buf, sizeof(buf), tr(Str::PollEveryFmt),
           static_cast<unsigned>(prefs_.batteryPollS()));
  add(Str::SetBatteryPoll, buf, false);
  if (eego::FrontlightManager::instance().present()) {
    add(Str::SetFrontlight, frontlightOn_ ? tr(Str::On) : tr(Str::Off), false);
  } else {
    add(Str::SetFrontlight, tr(Str::FrontlightMissing), false);
  }
  snprintf(buf, sizeof(buf), tr(Str::MutesFmt),
           static_cast<unsigned>(muteRuleCount_));  // store 规则数（M21）
  add(Str::SetMutes, buf, true);
  add(Str::SetPollNow, "", true);
  std::string url = tr(Str::WifiDisconnected);
  const eego::net::NetStatusSnapshot& ns = netStatus();
  if (!ns.wifi.ip.empty()) {
    url = "http://" + ns.wifi.ip + "/";
  }
  add(Str::SetAdmin, url, false);
  const esp_app_desc_t* d = esp_app_get_description();
  add(Str::Version, std::string(d->version), false);
}

}  // namespace panel
