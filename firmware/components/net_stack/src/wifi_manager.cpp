// SPDX-License-Identifier: GPL-3.0-or-later
#include "net_stack/wifi_manager.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "net_stack/wifi_list.h"
#include "sdkconfig.h"
#include "uc8279c_display.h"

namespace eego::net {
static const char* TAG = "net.wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WIFI_ASSOC_BIT BIT2
static constexpr uint32_t kFastConnectTimeoutMs = 6000;
static constexpr uint32_t kRelinkTimeoutMs = 10000;
static constexpr uint16_t kWifiStaInactiveS = 15;
static constexpr int64_t kRfQuietMaxWaitMs = 4000;
#ifdef CONFIG_ESP_PHY_MAX_WIFI_TX_POWER
static constexpr int8_t kWantTxPowerQdbm = CONFIG_ESP_PHY_MAX_WIFI_TX_POWER * 4;
#else
static constexpr int8_t kWantTxPowerQdbm = 80;
#endif

const char* wifiStateName(WifiState s) {
  switch (s) {
    case WifiState::Disconnected: return "disconnected";
    case WifiState::Connecting: return "connecting";
    case WifiState::Connected: return "connected";
    case WifiState::Failed: return "failed";
    case WifiState::OfflineMode: return "offline";
  }
  return "unknown";
}

#ifndef EEGO_WIFI_COUNTRY_CODE
#define EEGO_WIFI_COUNTRY_CODE "CN"
#endif

static int64_t nowMs() { return esp_timer_get_time() / 1000; }

namespace {
struct MutexLock {
  SemaphoreHandle_t m;
  explicit MutexLock(SemaphoreHandle_t mutex) : m(mutex) {
    if (m) xSemaphoreTake(m, portMAX_DELAY);
  }
  ~MutexLock() {
    if (m) xSemaphoreGive(m);
  }
};
struct RadioLock {
  SemaphoreHandle_t m;
  bool held = false;
  RadioLock(SemaphoreHandle_t mutex, TickType_t wait) : m(mutex) {
    held = m == nullptr || xSemaphoreTakeRecursive(m, wait) == pdTRUE;
  }
  ~RadioLock() {
    if (m && held) xSemaphoreGiveRecursive(m);
  }
};
}  // namespace

WifiManager& WifiManager::instance() {
  static WifiManager inst;
  return inst;
}

WifiManager::WifiManager()
    : radio_mutex_(xSemaphoreCreateRecursiveMutex()), diag_mutex_(xSemaphoreCreateMutex()) {}

WifiDiagSnapshot WifiManager::diagnostics() const {
  MutexLock lock(diag_mutex_);
  WifiDiagSnapshot snap = diag_.snapshot();
  snap.failed_passes = fallback_policy_.failedPasses();
  return snap;
}

bool WifiManager::apFallbackDue(int64_t now_ms) const {
  MutexLock lock(diag_mutex_);
  return fallback_policy_.due(now_ms, diag_.snapshot().networks.size());
}

uint8_t WifiManager::bestSavedChannel() const {
  MutexLock lock(diag_mutex_);
  uint8_t channel = 0;
  int best = -128;
  for (const auto& n : diag_.snapshot().networks) {
    if (n.seen && n.channel != 0 && n.rssi > best) {
      best = n.rssi;
      channel = n.channel;
    }
  }
  return channel;
}

void WifiManager::syncSavedNetworks(const std::vector<WifiAccount>& accounts) {
  std::vector<std::string> ssids;
  ssids.reserve(accounts.size());
  for (const auto& a : accounts) ssids.push_back(a.ssid);
  MutexLock lock(diag_mutex_);
  diag_.syncSaved(ssids);
}

void WifiManager::diagAttempt(const std::string& ssid) {
  MutexLock lock(diag_mutex_);
  diag_.recordAttempt(ssid, nowMs());
}

void WifiManager::diagResult(const std::string& ssid, WifiAttemptResult result, uint8_t reason) {
  MutexLock lock(diag_mutex_);
  diag_.recordResult(ssid, result, reason, nowMs());
}

std::string WifiManager::summary() const {
  wifi_mode_t mode = WIFI_MODE_NULL;
  const bool have_mode = initialized_ && esp_wifi_get_mode(&mode) == ESP_OK;
  const char* mode_name = "off";
  if (have_mode) {
    switch (mode) {
      case WIFI_MODE_STA: mode_name = "STA"; break;
      case WIFI_MODE_AP: mode_name = "AP"; break;
      case WIFI_MODE_APSTA: mode_name = "APSTA"; break;
      default: mode_name = "NULL"; break;
    }
  }
  char country[4] = "--";
  if (initialized_) esp_wifi_get_country_code(country);
  const int tx = tx_power_qdbm_;
  char buf[200];
  snprintf(buf, sizeof(buf),
           "mode=%s started=%d state=%s ps=%s bw=HT20 tx=%d.%02ddBm inactive=%us country=%s "
           "fast_hint=%s",
           mode_name, started_ ? 1 : 0, wifiStateName(state_),
           (ps_enabled_ ? "min_modem(battery-interactive)" : "none"), tx / 4, (tx % 4) * 25,
           (unsigned)kWifiStaInactiveS, country,
           fast_hint_.valid ? "pending" : (fast_hint_used_ ? "used" : "no"));
  return buf;
}

esp_err_t WifiManager::init() {
  if (initialized_) return ESP_OK;

  esp_err_t err = esp_netif_init();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
    return err;
  }
  err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "event_loop_create failed: %s", esp_err_to_name(err));
    return err;
  }

  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  err = esp_wifi_init(&cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
    return err;
  }
  err = esp_wifi_set_country_code(EEGO_WIFI_COUNTRY_CODE, true);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "set country %s failed: %s", EEGO_WIFI_COUNTRY_CODE, esp_err_to_name(err));
  }
  err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandler, this);
  if (err != ESP_OK) return err;
  err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifiEventHandler, this);
  if (err != ESP_OK) return err;

  err = esp_wifi_set_mode(WIFI_MODE_STA);
  if (err != ESP_OK) return err;
  err = esp_wifi_start();
  if (err != ESP_OK) return err;
  started_ = true;

  applyRadioTuning();

  event_group_ = xEventGroupCreate();
  if (!event_group_) return ESP_ERR_NO_MEM;

  initialized_ = true;
  ESP_LOGI(TAG, "wifi initialized (STA mode)");
  return ESP_OK;
}

esp_err_t WifiManager::ensureStarted() {
  if (!initialized_) return init();
  if (started_) return ESP_OK;
  const esp_err_t err = esp_wifi_start();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
    return err;
  }
  started_ = true;
  applyRadioTuning();
  ESP_LOGI(TAG, "wifi driver started");
  return ESP_OK;
}

esp_err_t WifiManager::restartDriver() {
  if (!initialized_) return init();
  connecting_ = false;
  esp_wifi_disconnect();
  esp_wifi_stop();
  started_ = false;
  setState(WifiState::Disconnected);
  driver_restarts_++;
  ESP_LOGW(TAG, "wifi driver restart #%u", (unsigned)driver_restarts_);
  return ensureStarted();
}

void WifiManager::setPowerSave(bool enable) {
  ps_enabled_ = enable;
  if (initialized_ && started_) applyPowerSave();
}

void WifiManager::applyPowerSave() {
  const bool ps = ps_enabled_ && state_ == WifiState::Connected;
  esp_wifi_set_ps(ps ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
}

void WifiManager::checkTxPower(const char* when) {
  int8_t q = 0;
  if (esp_wifi_get_max_tx_power(&q) != ESP_OK) return;
  const bool at_start = strcmp(when, "start") == 0;
  if (at_start && q < kWantTxPowerQdbm) {
    ESP_LOGW(TAG, "max tx power %d.%02d dBm below configured %d dBm; restoring", q / 4,
             (q % 4) * 25, kWantTxPowerQdbm / 4);
    const esp_err_t err = esp_wifi_set_max_tx_power(kWantTxPowerQdbm);
    if (err != ESP_OK) ESP_LOGW(TAG, "set_max_tx_power failed: %s", esp_err_to_name(err));
    esp_wifi_get_max_tx_power(&q);
  }
  tx_power_qdbm_ = q;
  ESP_LOGI(TAG, "max tx power %d.%02d dBm (%s, country %s)", q / 4, (q % 4) * 25, when,
           EEGO_WIFI_COUNTRY_CODE);
}

void WifiManager::applyRadioTuning() {
  if (!started_) return;
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) != ESP_OK) return;
  const bool sta = mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA;
  const bool ap = mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA;
  if (sta) {
    esp_err_t err = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
    if (err != ESP_OK) ESP_LOGW(TAG, "set STA HT20 failed: %s", esp_err_to_name(err));
    err = esp_wifi_set_inactive_time(WIFI_IF_STA, kWifiStaInactiveS);
    if (err != ESP_OK) ESP_LOGW(TAG, "set STA inactive time failed: %s", esp_err_to_name(err));
  }
  if (ap) {
    const esp_err_t err = esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
    if (err != ESP_OK) ESP_LOGW(TAG, "set AP HT20 failed: %s", esp_err_to_name(err));
  }
  applyPowerSave();
  checkTxPower("start");
}

void WifiManager::waitRfQuiet(const char* what) {
  if (!eego::Uc8279cDisplay::refreshActive()) return;
  const int64_t start = nowMs();
  while (eego::Uc8279cDisplay::refreshActive() && nowMs() - start < kRfQuietMaxWaitMs) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  ESP_LOGI(TAG, "%s deferred %lldms for e-ink refresh", what, (long long)(nowMs() - start));
}

void WifiManager::setState(WifiState s) {
  if (state_ == s) return;
  state_ = s;
  ESP_LOGI(TAG, "state -> %s", wifiStateName(s));
}

void WifiManager::wifiEventHandler(void* arg, esp_event_base_t base, int32_t id, void* data) {
  WifiManager* self = static_cast<WifiManager*>(arg);

  if (base == WIFI_EVENT) {
    switch (id) {
      case WIFI_EVENT_STA_CONNECTED: {
        wifi_event_sta_connected_t* evt = (wifi_event_sta_connected_t*)data;
        self->current_ssid_.assign((const char*)evt->ssid,
                                   strnlen((const char*)evt->ssid, 32));
        self->last_ap_.ssid = self->current_ssid_;
        memcpy(self->last_ap_.bssid, evt->bssid, sizeof(self->last_ap_.bssid));
        self->last_ap_.channel = evt->channel;
        self->last_ap_.valid = evt->channel != 0;
        self->assoc_at_ms_ = static_cast<uint32_t>(nowMs());
        if (self->event_group_) xEventGroupSetBits(self->event_group_, WIFI_ASSOC_BIT);
        break;
      }
      case WIFI_EVENT_STA_BEACON_TIMEOUT:
        self->beacon_timeouts_ = self->beacon_timeouts_ + 1;
        ESP_LOGW(TAG, "beacon timeout #%u (weak signal?)", (unsigned)self->beacon_timeouts_);
        break;
      case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t* evt = (wifi_event_sta_disconnected_t*)data;
        ESP_LOGW(TAG, "STA disconnected from %.*s, reason=%d (%s)",
                 (int)strnlen((const char*)evt->ssid, sizeof(evt->ssid)), (const char*)evt->ssid,
                 (int)evt->reason, wifiReasonName(evt->reason));
        self->last_reason_ = evt->reason;
        self->last_disconnect_reason_ = evt->reason;
        if (self->connecting_) {
          xEventGroupSetBits(self->event_group_, WIFI_FAIL_BIT);
        } else {
          if (evt->reason == WIFI_REASON_BEACON_TIMEOUT && self->state_ == WifiState::Connected) {
            self->relink_pending_ = true;
          }
          self->setState(WifiState::Disconnected);
        }
        break;
      }
      default:
        break;
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t* evt = (ip_event_got_ip_t*)data;
    ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&evt->ip_info.ip));
    if (self->connecting_) xEventGroupSetBits(self->event_group_, WIFI_CONNECTED_BIT);
    self->setState(WifiState::Connected);
  }
}

void WifiManager::setFastConnectHint(const ApHint& hint) { fast_hint_ = hint; }

ApHint WifiManager::lastConnectedAp() const { return last_ap_; }

WifiState WifiManager::connect(const WifiAccount& acc, uint32_t timeout_ms, const ApHint* hint) {
  RadioLock radio(radio_mutex_, portMAX_DELAY);
  if (ensureStarted() != ESP_OK) {
    setState(WifiState::Failed);
    return state_;
  }

  const bool fast = hint != nullptr && hint->valid && hint->channel != 0;
  const bool was_linked = state_ == WifiState::Connected;
  esp_wifi_disconnect();
  if (!fast || was_linked) vTaskDelay(pdMS_TO_TICKS(200));
  xEventGroupClearBits(event_group_, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT | WIFI_ASSOC_BIT);
  last_reason_ = 0;

  esp_wifi_set_ps(WIFI_PS_NONE);

  wifi_config_t cfg = {};
  memcpy(cfg.sta.ssid, acc.ssid.data(), std::min(acc.ssid.size(), sizeof(cfg.sta.ssid)));
  memcpy(cfg.sta.password, acc.password.data(),
         std::min(acc.password.size(), sizeof(cfg.sta.password) - 1));
  cfg.sta.threshold.authmode = acc.password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_PSK;
  cfg.sta.pmf_cfg.capable = true;
  cfg.sta.pmf_cfg.required = false;
  cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
  cfg.sta.failure_retry_cnt = 2;
  cfg.sta.listen_interval = 3;
  cfg.sta.rm_enabled = 1;
  cfg.sta.btm_enabled = 1;
  if (fast) {
    cfg.sta.bssid_set = true;
    memcpy(cfg.sta.bssid, hint->bssid, sizeof(cfg.sta.bssid));
    cfg.sta.channel = hint->channel;
    cfg.sta.scan_method = WIFI_FAST_SCAN;
  } else {
    cfg.sta.bssid_set = false;
    cfg.sta.channel = 0;
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  }

  esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "set_config failed: %s", esp_err_to_name(err));
    setState(WifiState::Failed);
    return state_;
  }

  current_ssid_ = acc.ssid;
  connecting_ = true;
  setState(WifiState::Connecting);
  diagAttempt(acc.ssid);

  waitRfQuiet("connect");
  const int64_t start_ms = nowMs();
  err = esp_wifi_connect();
  if (err != ESP_OK) {
    connecting_ = false;
    ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
    diagResult(acc.ssid, WifiAttemptResult::Failed, 0);
    setState(WifiState::Failed);
    return state_;
  }

  auto assocMs = [&](EventBits_t b) -> int64_t {
    if (!(b & WIFI_ASSOC_BIT)) return -1;
    const int32_t delta =
        static_cast<int32_t>(assoc_at_ms_ - static_cast<uint32_t>(start_ms));
    return start_ms + (delta < 0 ? 0 : delta);
  };
  int64_t deadline = wifiConnectDeadlineMs(start_ms, timeout_ms, -1);
  EventBits_t bits = 0;
  for (;;) {
    const int64_t now = nowMs();
    const int64_t wait = deadline > now ? deadline - now : 0;
    bits = xEventGroupWaitBits(event_group_, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
                               pdMS_TO_TICKS(static_cast<uint32_t>(wait)));
    if (bits & (WIFI_CONNECTED_BIT | WIFI_FAIL_BIT)) break;
    const int64_t assoc = assocMs(bits);
    const int64_t extended = wifiConnectDeadlineMs(start_ms, timeout_ms, assoc);
    if (assoc < 0 || extended <= deadline || extended <= nowMs()) break;
    ESP_LOGW(TAG, "%s associated %lldms ago, still no IP (weak link?); waiting for DHCP up to %llds",
             acc.ssid.c_str(), (long long)(nowMs() - assoc), (long long)(extended - nowMs()) / 1000);
    deadline = extended;
  }
  connecting_ = false;
  const int64_t done_ms = nowMs();
  const int64_t assoc_ms = assocMs(xEventGroupGetBits(event_group_));

  if (bits & WIFI_CONNECTED_BIT) {
    const int64_t assoc_took = assoc_ms >= 0 ? assoc_ms - start_ms : -1;
    const int64_t dhcp_took = assoc_ms >= 0 ? done_ms - assoc_ms : -1;
    if (dhcp_took > kWifiDhcpSlowMs) {
      ESP_LOGW(TAG, "connected to %s: assoc %lldms, slow DHCP %lldms", acc.ssid.c_str(),
               (long long)assoc_took, (long long)dhcp_took);
    } else {
      ESP_LOGI(TAG, "connected to %s: assoc %lldms, dhcp %lldms", acc.ssid.c_str(),
               (long long)assoc_took, (long long)dhcp_took);
    }
    diagResult(acc.ssid, WifiAttemptResult::Connected, 0);
    {
      MutexLock lock(diag_mutex_);
      fallback_policy_.onConnected();
      roam_policy_.onConnected();
      last_assoc_ms_ = static_cast<int32_t>(assoc_took);
      last_dhcp_ms_ = static_cast<int32_t>(dhcp_took);
    }
    applyPowerSave();
    checkTxPower("connected");
    return state_;
  }
  if (!(bits & WIFI_FAIL_BIT)) {
    esp_wifi_disconnect();
    last_reason_ = 0;
    if (assoc_ms >= 0) {
      ESP_LOGW(TAG, "connect timeout to %s: associated after %lldms but no IP after %lldms",
               acc.ssid.c_str(), (long long)(assoc_ms - start_ms), (long long)(done_ms - assoc_ms));
    } else {
      ESP_LOGW(TAG, "connect timeout to %s (not associated)", acc.ssid.c_str());
    }
    diagResult(acc.ssid, WifiAttemptResult::Timeout, 0);
  } else {
    ESP_LOGW(TAG, "connect failed to %s, reason=%d (%s)", acc.ssid.c_str(), (int)last_reason_,
             wifiReasonName(last_reason_));
    diagResult(acc.ssid, WifiAttemptResult::Failed, last_reason_);
  }
  setState(WifiState::Failed);
  return state_;
}

WifiState WifiManager::connectMulti(const std::vector<WifiAccount>& accounts) {
  RadioLock radio(radio_mutex_, portMAX_DELAY);
  syncSavedNetworks(accounts);
  if (ensureStarted() != ESP_OK) {
    setState(WifiState::OfflineMode);
    return state_;
  }
  if (accounts.empty()) {
    ESP_LOGW(TAG, "no wifi account configured, offline mode");
    setState(WifiState::OfflineMode);
    return state_;
  }

  ApHint hint;
  uint32_t hint_timeout = kFastConnectTimeoutMs;
  bool boot_hint = false;
  const bool relink = relink_pending_;
  relink_pending_ = false;
  if (fast_hint_.valid) {
    hint = fast_hint_;
    fast_hint_ = ApHint{};
    boot_hint = true;
  } else if (relink && last_ap_.valid) {
    hint = last_ap_;
    hint_timeout = kRelinkTimeoutMs;
  }
  if (hint.valid) {
    for (const WifiAccount& account : accounts) {
      if (account.ssid != hint.ssid) continue;
      if (boot_hint) fast_hint_used_ = true;
      ESP_LOGI(TAG, "%s %s (ch %u)", boot_hint ? "fast connect" : "relink after beacon loss",
               account.ssid.c_str(), (unsigned)hint.channel);
      if (connect(account, hint_timeout, &hint) == WifiState::Connected) return state_;
      ESP_LOGW(TAG, "%s failed; falling back to scan", boot_hint ? "fast connect" : "relink");
      break;
    }
  }

  esp_wifi_disconnect();

  std::vector<WifiScanHit> hits;
  scanLocked(&hits);
  ESP_LOGI(TAG, "scan found %u APs; %u saved networks", (unsigned)hits.size(),
           (unsigned)accounts.size());
  {
    const WifiDiagSnapshot d = diagnostics();
    for (const auto& n : d.networks) {
      if (n.seen) {
        ESP_LOGI(TAG, "  saved %s: seen rssi=%d ch=%u auth=%s", n.ssid.c_str(), (int)n.rssi,
                 (unsigned)n.channel, wifiAuthModeName(n.authmode));
      } else {
        ESP_LOGW(TAG, "  saved %s: NOT seen in scan (hidden / 5GHz-only / out of range?)",
                 n.ssid.c_str());
      }
    }
  }

  const std::vector<WifiAttempt> plan = planWifiAttempts(accounts, &hits);
  size_t skip_index = SIZE_MAX;
  for (size_t n = 0; n < plan.size(); n++) {
    const WifiAttempt& attempt = plan[n];
    if (attempt.index == skip_index) continue;
    const WifiAccount& account = accounts[attempt.index];
    ESP_LOGI(TAG, "connecting #%u %s (attempt %u/%u, timeout=%lums)",
             (unsigned)attempt.index + 1, account.ssid.c_str(), (unsigned)n + 1,
             (unsigned)plan.size(), (unsigned long)attempt.timeout_ms);
    if (connect(account, attempt.timeout_ms) == WifiState::Connected) {
      ESP_LOGI(TAG, "wifi connected: #%u %s rssi=%d", (unsigned)attempt.index + 1,
               account.ssid.c_str(), (int)rssi());
      return state_;
    }
    if (last_reason_ == WIFI_REASON_NO_AP_FOUND) skip_index = attempt.index;
  }

  bool saved_visible = false;
  {
    MutexLock lock(diag_mutex_);
    for (const auto& n : diag_.snapshot().networks) saved_visible = saved_visible || n.seen;
    fallback_policy_.onPassFailed(nowMs(), saved_visible);
  }
  ESP_LOGW(TAG, "all %u wifi networks failed (pass %u, saved visible=%d), offline mode",
           (unsigned)accounts.size(), (unsigned)fallback_policy_.failedPasses(),
           saved_visible ? 1 : 0);
  setState(WifiState::OfflineMode);
  return state_;
}

bool WifiManager::scanLocked(std::vector<WifiScanHit>* hits) {
  waitRfQuiet("scan");
  wifi_scan_config_t scan_cfg = {};
  scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  scan_cfg.show_hidden = true;
  std::vector<WifiScanEntry> entries;
  uint16_t ap_num = 0;
  bool ok = esp_wifi_scan_start(&scan_cfg, true) == ESP_OK &&
            esp_wifi_scan_get_ap_num(&ap_num) == ESP_OK;
  if (ok && ap_num > 0) {
    wifi_ap_record_t* aps = (wifi_ap_record_t*)malloc(sizeof(wifi_ap_record_t) * ap_num);
    if (aps == nullptr) {
      ok = false;
    } else if (esp_wifi_scan_get_ap_records(&ap_num, aps) == ESP_OK) {
      entries.reserve(ap_num);
      for (uint16_t i = 0; i < ap_num; i++) {
        WifiScanEntry e;
        e.ssid.assign((const char*)aps[i].ssid, strnlen((const char*)aps[i].ssid, 32));
        e.rssi = aps[i].rssi;
        e.channel = aps[i].primary;
        e.authmode = (int)aps[i].authmode;
        if (hits != nullptr && !e.ssid.empty()) hits->push_back({e.ssid, e.rssi});
        entries.push_back(std::move(e));
      }
    } else {
      ok = false;
    }
    free(aps);
  }
  if (!ok) ESP_LOGW(TAG, "wifi scan failed");
  MutexLock lock(diag_mutex_);
  diag_.recordScan(ok, std::move(entries), nowMs());
  return ok;
}

bool WifiManager::scanNow() {
  RadioLock radio(radio_mutex_, 0);
  if (!radio.held) return false;
  if (ensureStarted() != ESP_OK) return false;
  if (state_ == WifiState::Connecting) return false;
  return scanLocked(nullptr);
}

bool WifiManager::serviceLink(int64_t now_ms) {
  if (!initialized_ || state_ != WifiState::Connected) return false;
  {
    MutexLock lock(diag_mutex_);
    if (!link_stats_.sampleDue(now_ms)) return false;
  }
  const int r = rssi();
  MutexLock lock(diag_mutex_);
  link_stats_.add(now_ms, r);
  return roam_policy_.update(now_ms, r);
}

bool WifiManager::roam(const std::vector<WifiAccount>& accounts) {
  if (nowMs() < roam_defer_until_ms_.load()) return false;
  RadioLock radio(radio_mutex_, 0);
  if (!radio.held || state_ != WifiState::Connected) return false;
  {
    MutexLock lock(diag_mutex_);
    roam_policy_.onScanned(nowMs());
    roam_scans_++;
  }
  wifi_ap_record_t cur = {};
  if (esp_wifi_sta_get_ap_info(&cur) != ESP_OK) return false;
  const std::string ssid = current_ssid_;
  const WifiAccount* account = nullptr;
  for (const auto& a : accounts) {
    if (a.ssid == ssid) account = &a;
  }
  if (account == nullptr || ssid.empty()) return false;

  waitRfQuiet("roam scan");
  uint8_t ssid_buf[33] = {};
  memcpy(ssid_buf, ssid.data(), std::min<size_t>(ssid.size(), 32));
  wifi_scan_config_t scan_cfg = {};
  scan_cfg.ssid = ssid_buf;
  scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  std::vector<WifiBssCandidate> candidates;
  uint16_t ap_num = 0;
  if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK ||
      esp_wifi_scan_get_ap_num(&ap_num) != ESP_OK) {
    ESP_LOGW(TAG, "roam scan failed");
    return false;
  }
  if (ap_num > 0) {
    wifi_ap_record_t* aps = (wifi_ap_record_t*)malloc(sizeof(wifi_ap_record_t) * ap_num);
    if (aps == nullptr) return false;
    if (esp_wifi_scan_get_ap_records(&ap_num, aps) == ESP_OK) {
      for (uint16_t i = 0; i < ap_num; i++) {
        if (strncmp((const char*)aps[i].ssid, ssid.c_str(), 33) != 0) continue;
        WifiBssCandidate c;
        memcpy(c.bssid, aps[i].bssid, sizeof(c.bssid));
        c.channel = aps[i].primary;
        c.rssi = aps[i].rssi;
        candidates.push_back(c);
      }
    }
    free(aps);
  }
  const int idx = pickRoamTarget(cur.bssid, cur.rssi, candidates);
  if (idx < 0) {
    ESP_LOGI(TAG, "roam: no AP >= %d dB stronger for %s (current %d dBm, %u BSSIDs seen)",
             kWifiRoamMinGainDb, ssid.c_str(), (int)cur.rssi, (unsigned)candidates.size());
    return false;
  }
  const WifiBssCandidate& target = candidates[static_cast<size_t>(idx)];
  ApHint hint;
  hint.valid = true;
  hint.ssid = ssid;
  memcpy(hint.bssid, target.bssid, sizeof(hint.bssid));
  hint.channel = target.channel;
  ESP_LOGW(TAG, "roam: %s %d dBm (ch %u) -> %02x:%02x:%02x:%02x:%02x:%02x ch %u %d dBm",
           ssid.c_str(), (int)cur.rssi, (unsigned)cur.primary, target.bssid[0], target.bssid[1],
           target.bssid[2], target.bssid[3], target.bssid[4], target.bssid[5],
           (unsigned)target.channel, (int)target.rssi);
  if (connect(*account, kWifiVisibleRetryTimeoutMs, &hint) == WifiState::Connected) {
    MutexLock lock(diag_mutex_);
    roams_++;
    return true;
  }
  ESP_LOGW(TAG, "roam failed; reconnecting through saved networks");
  return false;
}

WifiManager::LinkInfo WifiManager::linkInfo() const {
  LinkInfo info;
  MutexLock lock(diag_mutex_);
  info.rssi = link_stats_.summary(nowMs());
  info.beacon_timeouts = beacon_timeouts_;
  info.roams = roams_;
  info.roam_scans = roam_scans_;
  info.last_assoc_ms = last_assoc_ms_;
  info.last_dhcp_ms = last_dhcp_ms_;
  info.tx_power_qdbm = tx_power_qdbm_;
  return info;
}

void WifiManager::disconnect() {
  connecting_ = false;
  if (initialized_) esp_wifi_disconnect();
  setState(WifiState::Disconnected);
}

void WifiManager::powerOff() {
  connecting_ = false;
  if (initialized_ && started_) {
    esp_wifi_disconnect();
    esp_wifi_stop();
  }
  started_ = false;
  setState(WifiState::Disconnected);
  ESP_LOGI(TAG, "wifi powered off");
}

int8_t WifiManager::rssi() const {
  if (!initialized_ || state_ != WifiState::Connected) return 0;
  wifi_ap_record_t ap;
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
  return 0;
}

std::string WifiManager::currentIp() const {
  if (!initialized_ || state_ != WifiState::Connected) return {};
  esp_netif_ip_info_t info;
  esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (netif && esp_netif_get_ip_info(netif, &info) == ESP_OK) {
    char buf[16];
    snprintf(buf, sizeof(buf), IPSTR, IP2STR(&info.ip));
    return buf;
  }
  return {};
}

}  // namespace eego::net
