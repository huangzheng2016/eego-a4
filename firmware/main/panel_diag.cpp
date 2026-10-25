// SPDX-License-Identifier: GPL-3.0-or-later
// diag 串口命令钩子实现（app_task 上下文；由 app_main 注册到 diag::Handlers）。
//
// secrets 边界：本文件只输出状态/计数/标识（ssid、源 id、告警标题截断），
// 绝不读取/打印 WiFi 密码、管理密码、ingest key、token 或 config 全文。
#include "panel_app.h"

#include <cstdio>
#include <cstring>
#include <strings.h>  // strcasecmp

#include "admin/admin.h"
#include "alert_core/types.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_flash.h"
#include "net_stack/net_stack.h"
namespace admin = eego::admin;

namespace panel {

namespace {

void printOk(bool ok) { printf("[%s] ", ok ? "PASS" : "FAIL"); }

const char* pageName(Page p) {
  switch (p) {
    case Page::Dashboard: return "dashboard";
    case Page::Onboarding: return "onboarding";
    case Page::AlertDetail: return "detail";
    case Page::Status: return "status";
    case Page::Settings: return "settings";
    case Page::Help: return "help";
    default: return "provision";
  }
}

}  // namespace

void PanelApp::diagStatus() {
  printf("boot: latch=%d touch=%d buttons=%d frontlight=%d rtc=%d sd=%d display=%d\n",
         deps_.boot.powerLatched ? 1 : 0, deps_.boot.touchReady ? 1 : 0,
         deps_.boot.buttonsReady ? 1 : 0, deps_.boot.frontlightPresent ? 1 : 0,
         deps_.boot.rtcReady ? 1 : 0, deps_.boot.sdReady ? 1 : 0,
         deps_.boot.displayReady ? 1 : 0);
  printf("power: pref=%d effective=%s charging=%d bat=%umV/%u%% page=%s\n",
         static_cast<int>(settings_.power_mode), mains_ ? "mains" : "battery",
         (charging_ || batCache_.charging) ? 1 : 0, batCache_.millivolts, batCache_.percent,
         pageName(page_));
  printf("uptime: %lld s, snapshot seq=%llu records=%u\n",
         static_cast<long long>(nowMs() / 1000), static_cast<unsigned long long>(snapshotSeq_),
         snapshot_ ? static_cast<unsigned>(snapshot_->records.size()) : 0u);
}

void PanelApp::diagBattery() {
  const eego::BatteryInfo bat = eego::PowerManager::instance().readBattery();
  printf("battery: %u mV, %u%%, charging=%d\n", bat.millivolts, bat.percent,
         bat.charging ? 1 : 0);
  // 电池模式跨深睡状态（硬件测试：唤醒次数/失败退避/上次刷屏/快连缓存）。
  const battery::RtcState& rtc = battery::rtcState();
  printf("battery-mode: wake=%s wakes=%u failures=%u frame_valid=%d refreshed=%llu "
         "full=%llu fast_since_full=%u bucket=%d ap=%d ch=%u\n",
         battery::wakeSourceName(wakeSource_), static_cast<unsigned>(rtc.wakes),
         static_cast<unsigned>(rtc.consecutive_failures), rtc.frame.valid,
         static_cast<unsigned long long>(rtc.frame.refresh_unix),
         static_cast<unsigned long long>(rtc.frame.full_unix),
         static_cast<unsigned>(rtc.frame.fast_since_full), rtc.battery_bucket, rtc.ap_valid,
         static_cast<unsigned>(rtc.ap_channel));
}

void PanelApp::diagRtc() {
  const eego::RtcTime t = rtc_.readTime();
  if (!t.valid && t.year == 0) {
    printf("rtc: read failed\n");
  } else {
    printf("rtc: %04u-%02u-%02u %02u:%02u:%02u wday=%u valid=%d\n", t.year, t.month, t.day,
           t.hour, t.minute, t.second, t.weekday, t.valid ? 1 : 0);
  }
  // 统一时间源状态（M21）：net_task 快照（含 SNTP 同步状态）+ 本地判定
  const eego::net::NetStatusSnapshot& ns = netStatus();
  printf("time: valid=%d now=%llu sntp_synced=%d last_sync_at=%llu\n", ns.time.valid ? 1 : 0,
         static_cast<unsigned long long>(ns.time.now), ns.time.sntp_synced ? 1 : 0,
         static_cast<unsigned long long>(ns.time.last_sync_at));
}

void PanelApp::diagSd() {
  eego::SdCard& sd = eego::SdCard::instance();
  printf("sd: ready=%d\n", sd.ready() ? 1 : 0);
  if (!sd.ready()) return;
  eego::SdCard::DirEntry entries[16];
  const int n = sd.listDir("/alertpanel", entries, 16);
  printf("sd: /alertpanel entries=%d\n", n);
  for (int i = 0; i < n; i++) {
    printf("  %s %s %u\n", entries[i].isDir ? "DIR " : "FILE", entries[i].name,
           static_cast<unsigned>(entries[i].size));
  }
}

void PanelApp::diagNet() {
  const eego::net::NetStatusSnapshot& ns = netStatus();
  printf("net: running=%d wifi_state=%d ssid=%s rssi=%d ip=%s backoff=%ums\n",
         ns.running ? 1 : 0, ns.wifi.state, ns.wifi.ssid.c_str(), ns.wifi.rssi,
         ns.wifi.ip.c_str(), static_cast<unsigned>(ns.wifi.reconnect_backoff_ms));
  printf("net: wifi last_disconnect_reason=%u reconnect_attempts=%u disconnects=%u "
         "driver_restarts=%u offline_ms=%u\n",
         static_cast<unsigned>(ns.wifi.last_disconnect_reason),
         static_cast<unsigned>(ns.wifi.reconnect_attempts),
         static_cast<unsigned>(ns.wifi.disconnects),
         static_cast<unsigned>(ns.wifi.driver_restarts),
         static_cast<unsigned>(ns.wifi.offline_ms));
  printf("net: pool_sessions=%u alerts_db attached=%d records=%u bytes=%u compactions=%u "
         "io_failures=%u\n",
         static_cast<unsigned>(ns.pool_sessions), ns.alert_db.attached ? 1 : 0,
         static_cast<unsigned>(ns.alert_db.records), static_cast<unsigned>(ns.alert_db.file_bytes),
         static_cast<unsigned>(ns.alert_db.compactions),
         static_cast<unsigned>(ns.alert_db.io_failures));
  // 数据源状态属于 dashboard（唯一取数模型）：见 `dashboard status`。
  for (const DashboardSourceInfo& src : dashboard_.sources()) {
    printf("  source %s enabled=%d data=%d offline=%d fail=%u http=%d\n", src.id.c_str(),
           src.enabled ? 1 : 0, src.has_data ? 1 : 0, src.offline ? 1 : 0,
           static_cast<unsigned>(src.consecutive_failures), src.last_http_status);
  }
}

void PanelApp::diagAlerts() {
  if (deps_.publisher != nullptr) {
    if (auto snap = deps_.publisher->acquire()) snapshot_ = snap;
  }
  if (snapshot_ == nullptr) {
    printf("alerts: no snapshot published yet\n");
    return;
  }
  const alert_core::AlertSummary& s = snapshot_->summary;
  printf("alerts: seq=%llu generated=%llu total=%u crit=%u warn=%u info=%u active=%u\n",
         static_cast<unsigned long long>(snapshot_->seq),
         static_cast<unsigned long long>(snapshot_->generated_at),
         static_cast<unsigned>(snapshot_->records.size()), static_cast<unsigned>(s.critical),
         static_cast<unsigned>(s.warning), static_cast<unsigned>(s.info),
         static_cast<unsigned>(s.active_total));
  size_t shown = 0;
  for (const auto& r : snapshot_->records) {
    if (shown >= 8) break;
    std::string title = r.title.empty() ? r.type : r.title;
    sanitizeClamped(title, 40);
    printf("  %.10s %-8s %-8s x%-4u %s%s\n", r.id.c_str(), alert_core::severityName(r.severity),
           alert_core::alertStateName(r.state), static_cast<unsigned>(r.occurrences),
           title.c_str(), r.muted ? " [muted]" : "");
    shown++;
  }
  if (snapshot_->records.size() > shown) {
    printf("  ... +%u more\n", static_cast<unsigned>(snapshot_->records.size() - shown));
  }
}

void PanelApp::diagInject(const char* json) {
  std::vector<alert_core::AlertEvent> events;
  alert_core::ValidationError err;
  if (!alert_core::eventsFromJsonText(json, events, err)) {
    printf("ERR inject: %s.%s\n", err.field.c_str(), err.message.c_str());
    return;
  }
  // 只入队：net_task 在 AlertStore 上下文执行摄入（M12 单属主模型）
  const alert_core::AlertSource src{alert_core::kSourceKindSerialInject,
                                    alert_core::kSourceKindSerialInject, "uart0"};
  const bool ok = eego::net::NetStack::instance().ingestExternal(events, src);
  printf("inject: queued %u event(s) -> %s\n", static_cast<unsigned>(events.size()),
         ok ? "ingested" : "unavailable");
}

void PanelApp::diagAckAll() {
  const eego::net::AlertActionResult r = eego::net::NetStack::instance().alertAction(
      {eego::net::AlertActionType::AckAll, "", "diag"});
  printf("ack-all: code=%s affected=%u%s%s\n",
         eego::net::toString(r.code), static_cast<unsigned>(r.affected),
         r.detail.empty() ? "" : " detail=", r.detail.c_str());
  markDirty();
}

void PanelApp::diagSmoke() {
  int pass = 0, fail = 0, warn = 0;
  auto check = [&pass, &fail](bool ok, const char* name, const char* detail) {
    printOk(ok);
    printf("%s: %s\n", name, detail);
    if (ok) {
      pass++;
    } else {
      fail++;
    }
  };

  // 1. 显示 + 灰阶链路（视觉确认：四级灰条 + 全量刷新）
  check(deps_.boot.displayReady, "display", deps_.boot.displayReady ? "ready" : "not ready");
  if (deps_.boot.displayReady && deps_.canvas != nullptr) {
    eego::PortraitCanvas& c = *deps_.canvas;
    c.clear();
    c.useUiContentRect();
    for (int i = 0; i < 4; i++) {
      c.fillRoundRectGray(static_cast<int16_t>(28 + i * 124), 300, 116, 120, 8,
                          static_cast<uint8_t>(i * 4));
    }
    eego::FontManager::instance().drawTextGray(c, 76, 460, "SMOKE GRAY 0/4/8/12", 2, 0);
    c.display(eego::RefreshMode::Full);
    c.waitRefreshComplete();
    printf("[PASS] display-gray: 4 gray bars drawn (visual check, full refresh)\n");
    pass++;
    markDirty(true);  // 恢复当前页
  }

  // 2. 字体
  const bool fontLoaded = eego::FontManager::instance().uiLoaded();
  if (fontLoaded) {
    check(true, "font", "SD font loaded");
  } else {
    printOk(true);
    printf("font: built-in 5x7 fallback (SD font absent)\n");
    warn++;
  }

  // 3. SD 读写
  eego::SdCard& sd = eego::SdCard::instance();
  if (sd.ready()) {
    static const char kSmokePath[] = "/alertpanel/.smoke";
    static const char kSmokeData[] = "smoke";
    const bool wr = sd.writeFile(kSmokePath, reinterpret_cast<const uint8_t*>(kSmokeData), 5);
    uint8_t rb[8] = {0};
    const size_t rn = wr ? sd.readFile(kSmokePath, rb, sizeof(rb)) : 0;
    const bool rm = wr ? sd.remove(kSmokePath) : false;
    check(wr && rn == 5 && memcmp(rb, kSmokeData, 5) == 0 && rm, "sd-rw",
          "write/read/remove roundtrip");
  } else {
    check(false, "sd-rw", "SD not mounted");
  }

  // 4. RTC
  const eego::RtcTime t = rtc_.readTime();
  if (t.year == 0) {
    check(false, "rtc", "read fail");
  } else if (!t.valid) {
    printf("[WARN] rtc: time not set or invalid (setup required)\n");
    warn++;
  } else {
    check(true, "rtc", "time valid");
  }

  // 5. 快照链路（publisher → net_task 发布）
  if (deps_.publisher != nullptr) {
    auto snap = deps_.publisher->acquire();
    check(snap != nullptr, "snapshot", snap ? "published" : "never published");
  } else {
    check(false, "snapshot", "publisher not wired");
  }

  // 6. net_task 查询链路
  const eego::net::NetStatusSnapshot& ns = netStatus();
  check(ns.running, "net", ns.running ? "net_task running" : "net_task not running");

  // 7. 配置
  check(admin::ConfigStore::instance().valid(), "config",
        admin::ConfigStore::instance().valid() ? "config.json valid" : "config invalid/bak");

  // 8. DeviceSettings 快照 + store 静音规则数
  std::vector<alert_core::MuteRule> mute_rules;
  eego::net::NetStack::instance().queryMuteRules(mute_rules);
  printf("[PASS] settings: lang=%s pmode=%d battery=%umin/stale %umin mute_rules=%u\n",
         settings_.language.c_str(), static_cast<int>(settings_.power_mode),
         static_cast<unsigned>(settings_.battery_refresh_min),
         static_cast<unsigned>(settings_.battery_max_stale_min),
         static_cast<unsigned>(mute_rules.size()));
  pass++;

  if (dashboard_.available()) {
    check(dashboard_.smoke(), "dashboard", "runtime checks complete");
    dashboard_.invalidateAll();
  } else {
    printf("[WARN] dashboard: not configured; onboarding screen active\n");
    warn++;
  }

  printf("smoke: pass=%d warn=%d fail=%d (output carries no secrets)\n", pass, warn, fail);
}

void PanelApp::diagVersion() {
  const esp_app_desc_t* d = esp_app_get_description();
  esp_chip_info_t chip;
  esp_chip_info(&chip);
  uint32_t flashBytes = 0;
  esp_flash_get_size(nullptr, &flashBytes);
  printf("version: %s %s (%s %s, IDF %s)\n", d->project_name, d->version, d->date, d->time,
         d->idf_ver);
  printf("chip: model=%d rev=%d cores=%d flash=%uMB psram=%uKB\n", chip.model, chip.revision,
         chip.cores, static_cast<unsigned>(flashBytes / (1024 * 1024)),
         static_cast<unsigned>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024));
  printf("framebuffer: %ux%u logical, %u bytes BW%s\n",
         static_cast<unsigned>(eego::Uc8279cDisplay::WIDTH),
         static_cast<unsigned>(eego::Uc8279cDisplay::HEIGHT),
         static_cast<unsigned>(eego::FRAMEBUFFER_SIZE),
         deps_.display != nullptr && deps_.display->hasGrayContent() ? " +gray planes" : "");
  printf("heap: free internal=%u KB\n",
         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
}

void PanelApp::diagLang(const char* arg) {
  Lang lang;
  if (!langFromString(arg, lang)) {
    printf("ERR usage: lang zh|en (current=%s)\n", settings_.language.c_str());
    return;
  }
  eego::settings::Settings next = settings_;
  next.language = langCode(lang);
  if (updateSettings(next)) printf("lang: -> %s\n", langCode(lang));
}

void PanelApp::diagPage(const char* arg) {
  Page target;
  if (strcasecmp(arg, "home") == 0) {
    goHome();
    printf("page: -> %s\n", pageName(page_));
    return;
  }
  if (strcasecmp(arg, "status") == 0) {
    target = Page::Status;
  } else if (strcasecmp(arg, "settings") == 0) {
    target = Page::Settings;
  } else if (strcasecmp(arg, "help") == 0) {
    target = Page::Help;
  } else if (strcasecmp(arg, "detail") == 0) {
    if (!dashboard_.available()) {
      printf("ERR page detail: dashboard not loaded\n");
      return;
    }
    const std::vector<DashboardAlertItem> items = dashboard_.alertItems({});
    if (items.empty()) {
      printf("ERR page detail: no alerts\n");
      return;
    }
    openDetail({}, items.front().identity);
    printf("page: -> %s (%u alert(s))\n", pageName(page_), static_cast<unsigned>(items.size()));
    return;
  } else if (strcasecmp(arg, "dashboard") == 0 || strcasecmp(arg, "dash") == 0) {
    if (!dashboard_.available()) {
      printf("ERR page dashboard: not loaded\n");
      return;
    }
    target = Page::Dashboard;
  } else {
    printf("ERR usage: page home|dashboard|detail|status|settings|help (current=%s)\n",
           pageName(page_));
    return;
  }
  gotoPage(target);
  printf("page: -> %s\n", pageName(page_));
}

void PanelApp::diagDashboard(const char* arg) { dashboard_.diag(arg); }

}  // namespace panel
