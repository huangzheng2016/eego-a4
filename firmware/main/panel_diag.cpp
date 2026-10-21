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
    case Page::Overview: return "s0-overview";
    case Page::Table: return "s1-table";
    case Page::Detail: return "s2-detail";
    case Page::Status: return "s3-status";
    case Page::Settings: return "s4-settings";
    case Page::Dashboard: return "dashboard";
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
  printf("net: sources=%u pool_sessions=%u\n", static_cast<unsigned>(ns.sources.size()),
         static_cast<unsigned>(ns.pool_sessions));
  for (const auto& s : ns.sources) {
    printf("  src %s kind=%s enabled=%d fail=%u http=%d ingested=%u cleared=%u skipped=%u\n",
           s.id.c_str(), s.kind.c_str(), s.enabled ? 1 : 0,
           static_cast<unsigned>(s.consecutive_failures), s.last_http_status,
           static_cast<unsigned>(s.events_ingested), static_cast<unsigned>(s.events_cleared),
           static_cast<unsigned>(s.events_skipped));
    if (!s.last_error.empty()) printf("    last_error: %s\n", s.last_error.c_str());
  }
}

void PanelApp::diagAlerts() {
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
  check(t.valid, "rtc", t.valid ? "time valid" : "VL flag set / read fail");

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
  printf("[PASS] settings: lang=%s pmode=%d bpoll=%us mute_rules=%u\n",
         settings_.language.c_str(), static_cast<int>(settings_.power_mode),
         static_cast<unsigned>(settings_.battery_poll_s), static_cast<unsigned>(mute_rules.size()));
  pass++;

  if (dashboard_.available()) {
    check(dashboard_.smoke(), "dashboard", "runtime checks complete");
    dashboard_.invalidateAll();
  } else {
    printf("[WARN] dashboard: not configured; legacy alert UI active\n");
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
  if (strcasecmp(arg, "s0") == 0) {
    target = Page::Overview;
  } else if (strcasecmp(arg, "s1") == 0) {
    target = Page::Table;
  } else if (strcasecmp(arg, "s2") == 0) {
    if (detailId_.empty()) {
      printf("ERR page s2 needs an open detail (use s1 first)\n");
      return;
    }
    target = Page::Detail;
  } else if (strcasecmp(arg, "s3") == 0) {
    target = Page::Status;
  } else if (strcasecmp(arg, "s4") == 0) {
    target = Page::Settings;
  } else if (strcasecmp(arg, "dashboard") == 0 || strcasecmp(arg, "dash") == 0) {
    if (!dashboard_.available()) {
      printf("ERR page dashboard: not loaded\n");
      return;
    }
    target = Page::Dashboard;
  } else {
    printf("ERR usage: page s0|s1|s2|s3|s4|dashboard (current=%s)\n", pageName(page_));
    return;
  }
  gotoPage(target);
  printf("page: -> %s\n", pageName(page_));
}

void PanelApp::diagDashboard(const char* arg) { dashboard_.diag(arg); }

}  // namespace panel
