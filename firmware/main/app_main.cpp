// SPDX-License-Identifier: GPL-3.0-or-later
//
//
//
//

#include "atomic_file.h"
#include "battery_mode.h"
#include "board/board_boot.h"
#include "eego_hal.h"
#include "eego_ui.h"

#include <vector>
#include <cstdio>
#include <cstring>
#include <strings.h>  // strcasecmp

#include <sys/time.h>

#include "admin/admin.h"
#include "admin/ble_bridge.h"
#include "admin/device_log.h"
#include "admin/time_policy.h"
#include "admin/dashboard_config.h"
#include "admin/display_snapshot.h"
#include "admin/render_preview.h"
#include "alert_core/persistence.h"
#include "alert_core/store.h"
#include "alert_core/type_registry.h"
#include "ble/ble_manager.h"
#include "diag/diag.h"
#include "device_settings/device_settings.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net_stack/uplink.h"
#include "net_stack/net_stack.h"
#include "net_stack/sd_alert_file_io.h"
#include "net_stack/wifi_list.h"
#include "input_merge.h"
#include "panel_app.h"
#include "time_source.h"

#if __has_include("local_secrets.h")
#include "local_secrets.h"
#endif

namespace admin = eego::admin;
namespace diag = eego::diag;

namespace {

const char* TAG = "app";

constexpr uint32_t kIdleWaitMs = 20;
constexpr uint32_t kSyncTouchPollMs = 10;
constexpr size_t kMaxTouchEventsPerLoop = 64;

TickType_t ticksAtLeastOne(uint32_t ms) {
  const TickType_t ticks = pdMS_TO_TICKS(ms);
  return ticks == 0 ? 1 : ticks;
}

eego::FrontlightOverride toFrontlightOverride(eego::settings::FrontlightHardware mode) {
  switch (mode) {
    case eego::settings::FrontlightHardware::Present:
      return eego::FrontlightOverride::Present;
    case eego::settings::FrontlightHardware::Absent:
      return eego::FrontlightOverride::Absent;
    case eego::settings::FrontlightHardware::Auto:
      break;
  }
  return eego::FrontlightOverride::Auto;
}

eego::ChargePolarity toChargePolarity(eego::settings::ChargeSignal mode) {
  switch (mode) {
    case eego::settings::ChargeSignal::ActiveHigh:
      return eego::ChargePolarity::ActiveHigh;
    case eego::settings::ChargeSignal::ActiveLow:
      return eego::ChargePolarity::ActiveLow;
    case eego::settings::ChargeSignal::Auto:
      break;
  }
  return eego::ChargePolarity::Auto;
}

void onDeviceSettingsChanged(const eego::settings::Settings& values, uint32_t changed,
                             void* ctx) {
  (void)ctx;
  if ((changed & eego::settings::kChangedChargeSignal) != 0) {
    eego::PowerManager::instance().setChargePolarity(toChargePolarity(values.charge_signal));
  }
  if ((changed & eego::settings::kChangedFrontlightHardware) == 0) return;
  eego::FrontlightManager& frontlight = eego::FrontlightManager::instance();
  frontlight.setOverride(toFrontlightOverride(values.frontlight_hardware));
  auto& settings = eego::settings::DeviceSettings::instance();
  eego::settings::Capabilities caps = settings.snapshot().capabilities;
  caps.frontlight = frontlight.frontlightAvailable();
  settings.setCapabilities(caps);
  if (caps.frontlight) {
    frontlight.setWarm(values.frontlight_warmth);
    frontlight.setBrightness(values.frontlight_enabled ? values.frontlight_brightness : 0);
  } else {
    frontlight.off();
  }
}

#if defined(EEGO_LOCAL_WIFI_LIST)
constexpr eego::net::WifiDefault kLocalWifiDefaults[] = EEGO_LOCAL_WIFI_LIST;
#elif defined(EEGO_LOCAL_WIFI_SSID)
#ifndef EEGO_LOCAL_WIFI_PASSWORD
#define EEGO_LOCAL_WIFI_PASSWORD ""
#endif
constexpr eego::net::WifiDefault kLocalWifiDefaults[] = {
    {EEGO_LOCAL_WIFI_SSID, EEGO_LOCAL_WIFI_PASSWORD}};
#endif
#if defined(EEGO_LOCAL_WIFI_LIST) || defined(EEGO_LOCAL_WIFI_SSID)
#define EEGO_HAS_LOCAL_WIFI 1
static_assert(eego::net::validWifiDefaults(kLocalWifiDefaults),
              "local_secrets.h WiFi defaults: 1..8 networks, unique SSID 1..32 bytes, "
              "password empty (open) or 8..63 bytes");
#endif
#ifdef EEGO_LOCAL_ADMIN_PASSWORD
static_assert(eego::net::wifiCstrLen(EEGO_LOCAL_ADMIN_PASSWORD) >= 8 &&
                  eego::net::wifiCstrLen(EEGO_LOCAL_ADMIN_PASSWORD) <= 128,
              "EEGO_LOCAL_ADMIN_PASSWORD must be 8..128 bytes");
#endif

#if defined(EEGO_HAS_LOCAL_WIFI) || defined(EEGO_LOCAL_ADMIN_PASSWORD)
void seedLocalCredentials(admin::ConfigStore& config) {
  (void)config;
#ifdef EEGO_HAS_LOCAL_WIFI
  std::vector<eego::net::WifiAccount> accounts;
  eego::net::CredentialStore::loadWifiAccounts(accounts);
#if defined(EEGO_LOCAL_WIFI_REPLACE)
  constexpr bool kReplace = true;
#else
  constexpr bool kReplace = false;
#endif
  if (eego::net::seedWifiDefaults(accounts, kLocalWifiDefaults,
                                  sizeof(kLocalWifiDefaults) / sizeof(kLocalWifiDefaults[0]),
                                  kReplace)) {
    const esp_err_t err = eego::net::CredentialStore::saveWifiAccounts(accounts);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "local WiFi defaults save failed: %s", esp_err_to_name(err));
    } else {
      ESP_LOGI(TAG, "ensured %u build-time WiFi default(s); saved list now %u network(s)",
               (unsigned)(sizeof(kLocalWifiDefaults) / sizeof(kLocalWifiDefaults[0])),
               (unsigned)accounts.size());
      std::vector<std::pair<std::string, std::string>> refs;
      for (size_t i = 0; i < accounts.size(); i++) {
        refs.emplace_back(accounts[i].ssid, "nvs:wifi/" + std::to_string(i));
      }
      alert_core::JsonValue dom = config.rawDom();
      std::string commit_error;
      if (!config.setWifiListInDom(dom, refs) || !config.commit(dom, commit_error)) {
        ESP_LOGW(TAG, "wifi_list config sync failed: %s", commit_error.c_str());
      }
    }
  }
  for (size_t i = 0; i < accounts.size(); i++) {
    ESP_LOGI(TAG, "wifi[%u] %s", (unsigned)i, accounts[i].ssid.c_str());  // SSID only
  }
  for (auto& account : accounts) eego::net::CredentialStore::secureErase(account.password);
#endif
#ifdef EEGO_LOCAL_ADMIN_PASSWORD
  if (!admin::AuthManager::instance().hasPassword()) {
    std::string password = EEGO_LOCAL_ADMIN_PASSWORD;
    const esp_err_t err = admin::AuthManager::instance().setPassword(password);
    eego::net::CredentialStore::secureErase(password);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "local admin password init failed: %s", esp_err_to_name(err));
    }
  }
#endif
}
#endif

void diagWifi(const char* arg) {
  eego::net::WifiManager& wifi = eego::net::WifiManager::instance();
  if (arg != nullptr && strcmp(arg, "retry") == 0) {
    const bool done = eego::net::NetStack::instance().reconnectNow(1000);
    printf("wifi retry: %s\n", done ? "queued" : "queued (net task busy connecting)");
    return;
  }
  if (arg != nullptr && strcmp(arg, "scan") == 0) {
    printf("wifi scan: %s\n", wifi.scanNow() ? "ok" : "busy/failed (showing last scan)");
  }
  {
    std::vector<eego::net::WifiAccount> saved;
    eego::net::CredentialStore::loadWifiAccounts(saved);
    wifi.syncSavedNetworks(saved);
    for (auto& account : saved) eego::net::CredentialStore::secureErase(account.password);
  }
  const eego::net::WifiDiagSnapshot d = wifi.diagnostics();
  const int64_t now = esp_timer_get_time() / 1000;
  printf("wifi: %s\n", wifi.summary().c_str());
  if (wifi.isConnected()) {
    printf("  connected: %s ip=%s rssi=%d\n", wifi.currentSsid().c_str(), wifi.currentIp().c_str(),
           (int)wifi.rssi());
  } else if (wifi.lastDisconnectReason() != 0) {
    printf("  last disconnect: %u %s - %s\n", (unsigned)wifi.lastDisconnectReason(),
           eego::net::wifiReasonName(wifi.lastDisconnectReason()),
           eego::net::wifiReasonText(wifi.lastDisconnectReason()));
  }
  printf("saved networks (%u), consecutive failed passes %u, fallback due %d:\n",
         (unsigned)d.networks.size(), (unsigned)d.failed_passes, wifi.apFallbackDue(now) ? 1 : 0);
  for (size_t i = 0; i < d.networks.size(); i++) {
    const eego::net::WifiNetworkDiag& n = d.networks[i];
    const long age = n.last_attempt_ms < 0 ? -1 : (long)((now - n.last_attempt_ms) / 1000);
    printf("  %u. %-32s %-10s tries=%u fails=%u last=%lds", (unsigned)i + 1, n.ssid.c_str(),
           eego::net::wifiAttemptResultName(n.result), (unsigned)n.attempts,
           (unsigned)n.failures, age);
    if (n.seen) {
      printf(" seen rssi=%d ch=%u %s\n", (int)n.rssi, (unsigned)n.channel,
             eego::net::wifiAuthModeName(n.authmode));
    } else {
      printf(" NOT seen\n");
    }
    if (n.failures > 0) {
      printf("     last reason %u %s - %s\n", (unsigned)n.last_reason,
             eego::net::wifiReasonName(n.last_reason), eego::net::wifiReasonText(n.last_reason));
      printf("     first-pass reason %u %s (%lds ago)\n", (unsigned)n.first_reason,
             eego::net::wifiReasonName(n.first_reason),
             (long)((now - n.first_failure_ms) / 1000));
    }
  }
  if (d.scan_ms < 0) {
    printf("scan: none yet (run `wifi scan`)\n");
    return;
  }
  printf("scan %s, %lds ago, %u APs:\n", d.scan_ok ? "ok" : "FAILED",
         (long)((now - d.scan_ms) / 1000), (unsigned)d.scan.size());
  for (const auto& e : d.scan) {
    printf("  %4d dBm ch%-2u %-18s %s\n", (int)e.rssi, (unsigned)e.channel,
           eego::net::wifiAuthModeName(e.authmode), e.ssid.c_str());
  }
}


eego::Uc8279cDisplay display;
eego::Gslx680Touch touch;
eego::PortraitCanvas canvas(display);
eego::board::BootStatus bootStatus;

alert_core::SnapshotPublisher publisher;
alert_core::TypeRegistry registry;

void entropyHook(uint8_t* out, size_t n) {
  size_t i = 0;
  while (i < n) {
    const uint32_t r = esp_random();
    const size_t chunk = (n - i < sizeof(r)) ? (n - i) : sizeof(r);
    memcpy(out + i, &r, chunk);
    i += chunk;
  }
}

alert_core::AlertStore store(registry, entropyHook);

eego::net::SdAlertFileIo alertFileIo;
alert_core::AlertDb alertDb(alertFileIo, "/sdcard/alertpanel/alerts.db");

panel::PanelApp app;


void tryLoadUiFont() {
  const std::string configured = admin::ConfigStore::instance().admin().font;
  if (!configured.empty()) {
    if (eego::FontManager::instance().loadUiFont(configured.c_str())) {
      ESP_LOGI(TAG, "UI font loaded (config): %s", configured.c_str());
      return;
    }
    ESP_LOGW(TAG, "configured font failed, fallback to embedded/SD: %s", configured.c_str());
  }
  if (eego::FontManager::instance().loadUiFontEmbedded()) return;
  if (!eego::SdCard::instance().ready()) return;
  // 16 x 264 B: on the heap, not the caller's stack.
  std::vector<eego::SdCard::DirEntry> entries(16);
  const int n = eego::SdCard::instance().listDir("/", entries.data(), 16);
  for (int i = 0; i < n; i++) {
    if (entries[i].isDir) continue;
    const char* dot = strrchr(entries[i].name, '.');
    if (!dot) continue;
    if (strcasecmp(dot, ".eefont") != 0 && strcasecmp(dot, ".bin") != 0) continue;
    char path[258];  // '/' + name(255) + NUL
    snprintf(path, sizeof(path), "/%.255s", entries[i].name);
    if (eego::FontManager::instance().loadUiFont(path)) {
      ESP_LOGI(TAG, "UI font loaded: %s", path);
      return;
    }
  }
  ESP_LOGI(TAG, "no SD font found, using built-in 5x7 fallback");
}


void onNetEvent(const eego::net::NetEvent& ev, void* ctx) {
  static_cast<panel::PanelApp*>(ctx)->notifyNetEvent(ev.wifi_state);
}

void onAdminEvent(const eego::admin::AdminEvent& ev, void* ctx) {
  int reason = -1;
  if (ev.reason != nullptr) {
    if (strcmp(ev.reason, "expired") == 0) reason = 0;
    else if (strcmp(ev.reason, "success") == 0) reason = 1;
    else if (strcmp(ev.reason, "manual") == 0) reason = 2;
  }
  static_cast<panel::PanelApp*>(ctx)->notifyAdminEvent(static_cast<int>(ev.type), reason,
                                                       ev.window_remaining_s);
}

admin::AlertActionResult adaptActionResult(const eego::net::AlertActionResult& r) {
  admin::AlertActionResult out;
  switch (r.code) {
    case eego::net::AlertActionResult::Code::Ok:
      out.code = admin::AlertActionResultCode::Ok;
      break;
    case eego::net::AlertActionResult::Code::NotFound:
      out.code = admin::AlertActionResultCode::NotFound;
      break;
    case eego::net::AlertActionResult::Code::InvalidState:
      out.code = admin::AlertActionResultCode::InvalidState;
      break;
    case eego::net::AlertActionResult::Code::InvalidAction:
      out.code = admin::AlertActionResultCode::InvalidAction;
      break;
    case eego::net::AlertActionResult::Code::NoStore:
      out.code = admin::AlertActionResultCode::NoStore;
      break;
    default:
      out.code = admin::AlertActionResultCode::Unavailable;
      break;
  }
  out.affected = r.affected;
  out.detail = r.detail;
  return out;
}


}  // namespace

extern "C" void app_main(void) {
  eego::admin::devlog::install();
  ESP_LOGI(TAG, "EEGO A4 monitoring panel boot");
  eego::admin::devlog::logBootReport();

  const panel::battery::WakeSource wake = panel::battery::wakeSource();
  const bool fastWake = wake == panel::battery::WakeSource::Timer;
  const bool keepPanel = panel::battery::deepSleepWake(wake) ||
                         (esp_reset_reason() == ESP_RST_SW &&
                          panel::battery::rtcState().frame.valid != 0);
  ESP_LOGI(TAG, "wake=%s fast=%d keep_panel=%d", panel::battery::wakeSourceName(wake),
           fastWake ? 1 : 0, keepPanel ? 1 : 0);

  eego::board::BootOptions bootOptions;
  bootOptions.touch = !fastWake;
  bootOptions.clearDisplay = !keepPanel;
  bootOptions.deferDisplay = fastWake;
  bootStatus = eego::board::beginStandardHardware(display, touch, bootOptions);

  if (panel::TimeSource::syncSystemFromRtc()) {
    ESP_LOGI(TAG, "system time set from RTC");
  }

  if (eego::SdCard::instance().ready() &&
      !eego::SdCard::instance().mkdir("/alertpanel")) {
    ESP_LOGE(TAG, "failed to create /sdcard/alertpanel");
  }
  if (eego::net::CredentialStore::ensureNvs() != ESP_OK) {
    ESP_LOGE(TAG, "NVS init failed; credentials/config persistence unavailable");
  }
  admin::AuthManager::instance().init();
  admin::ConfigStore& config = admin::ConfigStore::instance();
  config.init("/sdcard/alertpanel/config.json");
  if (!config.valid()) {
    ESP_LOGW(TAG, "config.json invalid (backup made), running on defaults");
  }
#if defined(EEGO_HAS_LOCAL_WIFI) || defined(EEGO_LOCAL_ADMIN_PASSWORD)
  seedLocalCredentials(config);
#endif
  admin::DashboardStore::instance().init("/sdcard/alertpanel/dashboard.json");
  const alert_core::JsonValue dom = config.rawDom();
  const alert_core::JsonValue* overrides = dom.find("alert_type_overrides");
  if (overrides != nullptr) {
    alert_core::ValidationError verr;
    if (!registry.applyOverridesJson(*overrides, verr)) {
      ESP_LOGW(TAG, "type overrides apply failed: %s.%s", verr.field.c_str(),
               verr.message.c_str());
    }
  }
  eego::admin::LogLevel logLevel;
  if (eego::admin::logLevelFromString(config.admin().log_level, logLevel)) {
    eego::admin::AdminLog::instance().init(logLevel);
  } else {
    eego::admin::AdminLog::instance().init();
  }

  auto& deviceSettings = eego::settings::DeviceSettings::instance();
  const esp_err_t settingsErr = deviceSettings.init();
  if (settingsErr != ESP_OK) {
    ESP_LOGE(TAG, "device settings init failed: %s", esp_err_to_name(settingsErr));
  }
  eego::PowerManager::instance().setChargePolarity(
      toChargePolarity(deviceSettings.snapshot().values.charge_signal));
#ifdef EEGO_LOCAL_POWER_MODE_MAINS
  {
    eego::settings::Settings next = deviceSettings.snapshot().values;
    next.power_mode = eego::settings::PowerMode::Mains;
    uint32_t changed = 0;
    std::string error;
    if (!deviceSettings.update(next, &changed, error)) {
      ESP_LOGE(TAG, "local power mode init failed: %s", error.c_str());
    }
  }
#endif
#ifdef EEGO_LOCAL_TIMEZONE_OFFSET_MIN
  {
    eego::settings::Settings next = deviceSettings.snapshot().values;
    next.timezone_offset_min = EEGO_LOCAL_TIMEZONE_OFFSET_MIN;
    uint32_t changed = 0;
    std::string error;
    if (deviceSettings.update(next, &changed, error)) {
      ESP_LOGI(TAG, "local timezone set to %d", next.timezone_offset_min);
    } else {
      ESP_LOGE(TAG, "local timezone init failed: %s", error.c_str());
    }
  }
#endif
  if (fastWake && panel::battery::rtcState().manual_sleep == 0) {
    const eego::BatteryInfo bat = eego::PowerManager::instance().readBattery();
    // A charge pin caught reporting charging on battery stays untrusted while it reads
    // charging, so the wake does not bounce into the always-on mains app.
    const bool charging = (eego::PowerManager::instance().isCharging() || bat.charging) &&
                          panel::battery::rtcState().charge_suspect == 0;
    if (panel::battery::mainsPowered(
            static_cast<uint8_t>(deviceSettings.snapshot().values.power_mode), charging,
            bat.millivolts)) {
      ESP_LOGI(TAG, "timer wake on external power: restarting into the full app");
      panel::battery::commitRtcState();
      esp_restart();
    }
  }
  // Hourly check from a critical-battery sleep: no radio, no panel, ~0.2 s awake. Stay
  // asleep until charging or back above kCriticalExitPct, then boot normally.
  if (fastWake && panel::battery::rtcState().low_battery_shown != 0) {
    eego::PowerManager& pm = eego::PowerManager::instance();
    const eego::BatteryInfo bat = pm.readBattery();
    const bool charging = pm.isCharging() || bat.charging;
    if (!charging && bat.millivolts > 0 && bat.percent < panel::battery::kCriticalExitPct) {
      ESP_LOGI(TAG, "critical battery recheck: %umV (%u%%), staying asleep", bat.millivolts,
               static_cast<unsigned>(bat.percent));
      panel::battery::commitRtcState();
      panel::battery::deepSleep(panel::battery::kCriticalRecheckS, false);
    }
    ESP_LOGI(TAG, "critical battery recheck: %umV (%u%%)%s, resuming", bat.millivolts,
             static_cast<unsigned>(bat.percent), charging ? " charging" : "");
  }
  eego::FrontlightManager::instance().setOverride(
      toFrontlightOverride(deviceSettings.snapshot().values.frontlight_hardware));
  eego::settings::Capabilities capabilities;
  capabilities.frontlight = eego::FrontlightManager::instance().frontlightAvailable();
  capabilities.frontlight_chip = eego::FrontlightManager::instance().lm3630aChipPresent();
  capabilities.battery = eego::PowerManager::instance().readBattery().millivolts > 0;
  capabilities.bluetooth = eego::ble::BleManager::compiled();
  capabilities.ble_admin = eego::ble::BleManager::compiled();
  capabilities.ble_observer = eego::ble::BleManager::compiled();
  capabilities.display = bootStatus.displayReady;
  capabilities.sd = bootStatus.sdReady;
  capabilities.rtc = bootStatus.rtcReady;
  deviceSettings.setCapabilities(capabilities);
  if (deviceSettings.subscribe(&onDeviceSettingsChanged, nullptr) < 0) {
    ESP_LOGW(TAG, "settings subscription table full (frontlight override needs reboot)");
  }
  const esp_err_t inputErr = admin::InputBindings::instance().init();
  if (inputErr != ESP_OK) {
    ESP_LOGE(TAG, "input bindings init failed: %s; using safe defaults",
             esp_err_to_name(inputErr));
  }

  tryLoadUiFont();

  const bool alertDbReady = eego::SdCard::instance().ready();
  if (alertDbReady) {
    if (eego::fsutil::recoverFile(alertDb.path())) {
      ESP_LOGW(TAG, "alerts.db restored from interrupted replace");
    }
    const size_t loaded = store.loadFrom(alertDb);
    ESP_LOGI(TAG, "alerts.db restored: %u record(s), %u dead line(s)",
             static_cast<unsigned>(loaded), static_cast<unsigned>(alertDb.deadLines()));
    if (alertDb.deadLines() > 0 && fastWake) {
      ESP_LOGI(TAG, "alerts.db boot compaction skipped on battery wake");
    } else if (alertDb.deadLines() > 0) {
      const std::string marker = alertDb.path() + ".compacting";
      const size_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      const size_t largestInternal =
          heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      constexpr size_t kBootCompactMinFree = 48 * 1024;
      constexpr size_t kBootCompactMinBlock = 16 * 1024;
      if (alertFileIo.exists(marker)) {
        alertFileIo.remove(marker);
        ESP_LOGW(TAG, "alerts.db boot compaction skipped: previous attempt did not finish");
      } else if (freeInternal < kBootCompactMinFree || largestInternal < kBootCompactMinBlock) {
        ESP_LOGW(TAG, "alerts.db boot compaction deferred: internal heap free=%u largest=%u",
                 static_cast<unsigned>(freeInternal), static_cast<unsigned>(largestInternal));
      } else {
        alertFileIo.writeAll(marker, "1");
        std::string compactErr;
        if (store.compactDb(compactErr)) {
          ESP_LOGI(TAG, "alerts.db compacted at boot: %u bytes",
                   static_cast<unsigned>(store.lastCompactBytes()));
        } else {
          ESP_LOGW(TAG, "alerts.db boot compaction failed: %s", compactErr.c_str());
        }
        alertFileIo.remove(marker);
      }
    }
  } else {
    ESP_LOGW(TAG, "SD not ready: alerts.db unavailable, running in-memory");
  }
  store.attachDb(&alertDb);

  if (panel::battery::deepSleepWake(wake)) {
    const panel::battery::RtcState& rtc = panel::battery::rtcState();
    if (rtc.ap_valid != 0) {
      eego::net::ApHint hint;
      hint.valid = true;
      hint.ssid = rtc.ap_ssid;
      memcpy(hint.bssid, rtc.ap_bssid, sizeof(hint.bssid));
      hint.channel = rtc.ap_channel;
      eego::net::WifiManager::instance().setFastConnectHint(hint);
    }
  }
  eego::net::NetStackConfig netCfg;
  netCfg.snapshot_publisher = &publisher;
  netCfg.rtc_write_fn = &panel::TimeSource::writeRtc;
  netCfg.time_valid_fn = &panel::TimeSource::valid;
  netCfg.alert_db = alertDbReady ? &alertDb : nullptr;
  const esp_err_t netErr =
      eego::net::NetStack::instance().start(netCfg, &store, &registry, &panel::TimeSource::now);
  if (netErr != ESP_OK) {
    ESP_LOGE(TAG, "net_task start failed: %s", esp_err_to_name(netErr));
  }
  if (!fastWake) {
    const esp_err_t bleErr =
        eego::ble::BleManager::instance().init();
    if (bleErr != ESP_OK) {
      ESP_LOGE(TAG, "BLE manager init failed: %s", esp_err_to_name(bleErr));
    } else {
      // Data sources can go through a computer agent linked over BLE (bleproxyagent).
      eego::net::uplink::Agent agent;
      agent.linked = [] { return eego::ble::BleManager::instance().agentLinked(); };
      agent.fetch = [](const std::string& head, const uint8_t* body, size_t body_len,
                       uint32_t wait_ms, std::string& resp_head, uint8_t** resp_body,
                       size_t* resp_body_len) {
        return eego::ble::BleManager::instance().proxyFetch(head, body, body_len, wait_ms, resp_head,
                                                            resp_body, resp_body_len);
      };
      eego::net::uplink::setAgent(agent);
    }
  }
  {
    std::vector<eego::net::WifiAccount> saved;
    eego::net::CredentialStore::loadWifiAccounts(saved);
    eego::net::WifiManager::instance().syncSavedNetworks(saved);
    std::string ssids;
    for (auto& account : saved) {
      if (!ssids.empty()) ssids += ", ";
      ssids += account.ssid;
      eego::net::CredentialStore::secureErase(account.password);
    }
#if defined(CONFIG_ESP_COEX_SW_COEXIST_ENABLE)
    constexpr const char* kCoex = "sw";
#else
    constexpr const char* kCoex = "off";
#endif
    const eego::ble::Status ble = eego::ble::BleManager::instance().status();
    ESP_LOGI(TAG, "wifi boot: saved=[%s] %s ble=%s/%s coex=%s", ssids.c_str(),
             eego::net::WifiManager::instance().summary().c_str(),
             ble.compiled ? "on" : "off", eego::ble::roleName(ble.role), kCoex);
  }

  {
    const std::vector<alert_core::MuteRule> mute_rules = config.muteRules();
    std::string mute_err;
    if (!mute_rules.empty() &&
        !eego::net::NetStack::instance().setMuteRules(mute_rules, mute_err)) {
      // net_task is often busy joining WiFi at boot; keep retrying in the background.
      ESP_LOGW(TAG, "mute rules restore deferred: %s", mute_err.c_str());
      auto* pending = new std::vector<alert_core::MuteRule>(mute_rules);
      xTaskCreate(
          [](void* arg) {
            auto* rules = static_cast<std::vector<alert_core::MuteRule>*>(arg);
            std::string err;
            bool ok = false;
            for (int i = 0; i < 24 && !ok; i++) {
              vTaskDelay(pdMS_TO_TICKS(5000));
              ok = eego::net::NetStack::instance().setMuteRules(*rules, err);
            }
            if (ok) ESP_LOGI(TAG, "mute rules restored (%u)", static_cast<unsigned>(rules->size()));
            else ESP_LOGE(TAG, "mute rules restore failed: %s", err.c_str());
            delete rules;
            vTaskDelete(nullptr);
          },
          "mute_restore", 4096, pending, 2, nullptr);
    }
  }

  eego::net::NetStack& net = eego::net::NetStack::instance();
  admin::setAlertSnapshotSource(net.alertSnapshotSource());
  admin::setAlertActionSink(
      [](admin::AlertActionKind kind, const std::string& id, const std::string& ack_by) {
        eego::net::AlertAction action;
        switch (kind) {
          case admin::AlertActionKind::Ack:
            action.type = eego::net::AlertActionType::Ack;
            break;
          case admin::AlertActionKind::AckAll:
            action.type = eego::net::AlertActionType::AckAll;
            break;
          default:
            action.type = eego::net::AlertActionType::Clear;
            break;
        }
        action.id = id;
        action.ack_by = ack_by;
        return adaptActionResult(eego::net::NetStack::instance().alertAction(action));
      });
  admin::setAdminCommandHandler(&admin::defaultCommandHandler, nullptr);
  admin::setAdminEventCallback(&onAdminEvent, &app);
  admin::setDisplaySnapshotProvider([](uint8_t* rows, size_t stride) {
    if (!bootStatus.displayReady) return false;
    for (int16_t y = 0; y < static_cast<int16_t>(admin::kSnapshotHeight); ++y) {
      uint8_t* row = rows + static_cast<size_t>(y) * stride;
      for (int16_t x = 0; x < static_cast<int16_t>(admin::kSnapshotWidth); ++x) {
        if (display.getPixel(x, y)) row[x / 8] |= static_cast<uint8_t>(0x80 >> (x & 7));
      }
    }
    return true;
  });
  admin::setRenderPreviewProvider([](const admin::RenderPreviewRequest& request, uint8_t* rows,
                                     size_t stride, admin::RenderPreviewResult& result) {
    return app.renderDashboardPreview(request, rows, stride, result);
  });
  net.setEventCallback(&onNetEvent, &app);
  admin::setTimeSetter([](int64_t unix_ms) {
    admin::TimeSetResult result;
    eego::net::NetStatusSnapshot ns;
    const bool ns_ok = eego::net::NetStack::instance().queryStatus(ns);
    const int64_t now_s = static_cast<int64_t>(time(nullptr));
    const bool valid = panel::TimeSource::valid();
    result.sntp_synced = ns_ok && ns.time.sntp_synced;
    const int64_t sync_age = (ns_ok && ns.time.last_sync_at > 0)
                                 ? now_s - static_cast<int64_t>(ns.time.last_sync_at)
                                 : -1;
    result.drift_s = unix_ms / 1000 - now_s;
    if (!admin::shouldApplyBrowserTime(valid, result.sntp_synced, sync_age, result.drift_s)) {
      return result;
    }
    timeval tv = {};
    tv.tv_sec = static_cast<time_t>(unix_ms / 1000);
    tv.tv_usec = static_cast<suseconds_t>((unix_ms % 1000) * 1000);
    if (settimeofday(&tv, nullptr) == 0) {
      result.applied = true;
      panel::TimeSource::writeRtc(static_cast<uint64_t>(tv.tv_sec));
    }
    return result;
  });

  if (fastWake) {
    ESP_LOGI(TAG, "battery wake: admin API not started");
  } else if (!admin::AuthManager::instance().hasPassword()) {
    ESP_LOGI(TAG, "no admin password: opening provisioning portal");
    if (admin::ProvisionPortal::instance().open() == ESP_OK) {
      app.enterProvision();
    }
  } else {
    admin::AdminConfig acfg;
    acfg.registry = &registry;
    const esp_err_t admErr = admin::startAdminApi(acfg);
    if (admErr != ESP_OK) {
      ESP_LOGW(TAG, "admin API start failed: %s", esp_err_to_name(admErr));
    }
  }

  if (!fastWake) {
    const esp_err_t bridgeErr = admin::startBleBridge();
    if (bridgeErr != ESP_OK && bridgeErr != ESP_ERR_NOT_SUPPORTED) {
      ESP_LOGW(TAG, "BLE bridge start failed: %s", esp_err_to_name(bridgeErr));
    }
  }

  diag::Handlers hooks;
  hooks.status = [] { app.diagStatus(); };
  hooks.battery = [] { app.diagBattery(); };
  hooks.rtc = [] { app.diagRtc(); };
  hooks.sd = [] { app.diagSd(); };
  hooks.net = [] { app.diagNet(); };
  hooks.alerts = [] { app.diagAlerts(); };
  hooks.inject = [](const char* json) { app.diagInject(json); };
  hooks.ack_all = [] { app.diagAckAll(); };
  hooks.smoke = [] { app.diagSmoke(); };
  hooks.version = [] { app.diagVersion(); };
  hooks.lang = [](const char* arg) { app.diagLang(arg); };
  hooks.page = [](const char* arg) { app.diagPage(arg); };
  hooks.dashboard = [](const char* arg) { app.diagDashboard(arg); };
  hooks.touch = [](const char* arg) { app.diagTouch(arg); };
  hooks.wifi = [](const char* arg) { diagWifi(arg); };
  if (!fastWake && !diag::start(hooks)) {
    ESP_LOGW(TAG, "diag start failed (serial diagnostics unavailable)");
  }

  panel::PanelApp::Deps deps;
  deps.display = &display;
  deps.touch = &touch;
  deps.canvas = &canvas;
  deps.boot = bootStatus;
  deps.publisher = &publisher;
  deps.registry = &registry;
  deps.fastWake = fastWake;
  app.begin(deps);

  SemaphoreHandle_t inputWake = xSemaphoreCreateBinary();
  eego::ButtonManager::instance().setWakeSemaphore(inputWake);
  if (bootStatus.touchReady) {
    const esp_err_t touchErr = touch.startPolling(inputWake);
    if (touchErr != ESP_OK) {
      ESP_LOGW(TAG, "touch task start failed (%s); polling synchronously",
               esp_err_to_name(touchErr));
    }
  }
  ESP_LOGI(TAG, "app_task running");
  eego::ButtonEvent bevt;
  panel::InputEventBatch inputEvents;
  while (true) {
    app.noteInputResume();
    inputEvents.clear();
    const bool touchTask = touch.pollingActive();
    for (size_t n = 0; n < kMaxTouchEventsPerLoop; ++n) {
      const eego::TouchEvent tev = touch.poll();
      if (tev.type == eego::TouchEventType::None) break;
      inputEvents.add(tev);
      if (!touchTask && !touch.hasPending()) break;
    }
    while (eego::ButtonManager::instance().poll(&bevt, 0)) {
      inputEvents.add(bevt);
    }
    inputEvents.dispatch(app);

    if (deviceSettings.snapshot().values.diagnostics_enabled) diag::service();

    admin::serviceCommands(0);

    app.service();

    if (touch.hasPending()) continue;
    const TickType_t wait = ticksAtLeastOne(touchTask ? kIdleWaitMs : kSyncTouchPollMs);
    if (inputWake != nullptr) {
      xSemaphoreTake(inputWake, wait);
    } else {
      vTaskDelay(wait);
    }
  }
}
