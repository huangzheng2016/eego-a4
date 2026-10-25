// SPDX-License-Identifier: GPL-3.0-or-later
// WiFi STA 状态机实现（改造自 eego-a4-weread wifi_manager.cpp，GPL-3.0）。
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

namespace eego::net {

static const char* TAG = "net.wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
static constexpr uint32_t kFastConnectTimeoutMs = 6000;

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

// 国家码：默认 CN（信道 1-13；ESP-IDF 默认 "01" 只主动扫描 1-11，12/13 信道上的
// 路由器可能扫不到或关联不稳）。可在构建时用 -DEEGO_WIFI_COUNTRY_CODE='"JP"' 等覆盖。
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
  char buf[160];
  snprintf(buf, sizeof(buf), "mode=%s started=%d state=%s ps=%s country=%s fast_hint=%s",
           mode_name, started_ ? 1 : 0, wifiStateName(state_),
           (ps_enabled_ ? "min_modem(after-connect)" : "none"), country,
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
  // 国家码在 start 前设置；ieee80211d=true：连上后跟随 AP 通告的国家信息。
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

  applyPowerSave();
  // TX 功率由 sdkconfig CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=20 控制，无需运行时设置
  // （esp_wifi_set_max_tx_power 单位是 0.25dBm，误传 20 会变 5dBm，weread 教训）。

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
  applyPowerSave();  // start 会把省电重置为默认值
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
  // weread 实证：esp_wifi_start() 会把省电重置为默认 MIN_MODEM，
  // 每次 start/reconnect 后必须重新应用。MIN_MODEM 只在已连上后启用：
  // 关联/4-way 握手/DHCP 期间 modem sleep 在弱信号下会导致握手超时。
  const bool ps = ps_enabled_ && state_ == WifiState::Connected;
  esp_wifi_set_ps(ps ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
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
        // 快连缓存：sys_evt 上下文只做小拷贝（读取方在连接完成后才读）。
        self->last_ap_.ssid = self->current_ssid_;
        memcpy(self->last_ap_.bssid, evt->bssid, sizeof(self->last_ap_.bssid));
        self->last_ap_.channel = evt->channel;
        self->last_ap_.valid = evt->channel != 0;
        break;
      }
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
  esp_wifi_disconnect();
  // 等底层完成断开，避免紧随其后的 esp_wifi_connect() 触发驱动竞态（weread 实证）。
  // 快连只发生在本次启动的首次连接（之前从未关联），无断开可等。
  if (!fast) vTaskDelay(pdMS_TO_TICKS(200));
  xEventGroupClearBits(event_group_, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
  last_reason_ = 0;

  // 关联/握手/DHCP 期间关闭省电（拿到 IP 后再按 setPowerSave 应用）。
  esp_wifi_set_ps(WIFI_PS_NONE);

  // 每次从零构造：快连失败后的常规尝试不会残留 bssid_set/channel。
  wifi_config_t cfg = {};
  // ssid 字段 32 字节、可不以 NUL 结尾（32 字节 SSID 合法）；password 64 字节。
  memcpy(cfg.sta.ssid, acc.ssid.data(), std::min(acc.ssid.size(), sizeof(cfg.sta.ssid)));
  memcpy(cfg.sta.password, acc.password.data(),
         std::min(acc.password.size(), sizeof(cfg.sta.password) - 1));
  // 认证阈值：有密码 → WPA_PSK（接受 WPA/WPA2/WPA3-PSK）；空密码 → OPEN（开放网络
  // 否则会被阈值过滤，报 NO_AP_FOUND_IN_AUTHMODE_THRESHOLD）。
  cfg.sta.threshold.authmode = acc.password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_PSK;
  cfg.sta.pmf_cfg.capable = true;   // v5.x 已恒为 true，显式写出
  cfg.sta.pmf_cfg.required = false;
  cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;  // WPA3/过渡模式：H2E 与 hunting-and-pecking 都接受
  cfg.sta.failure_retry_cnt = 2;            // 同一 SSID 多 AP 时失败后换下一个 BSSID
  if (fast) {
    // 固定 BSSID + 信道：驱动只在该信道探测该 AP，跳过全信道扫描。
    cfg.sta.bssid_set = true;
    memcpy(cfg.sta.bssid, hint->bssid, sizeof(cfg.sta.bssid));
    cfg.sta.channel = hint->channel;
    cfg.sta.scan_method = WIFI_FAST_SCAN;
  } else {
    // 全信道扫描后按信号选同名 AP（多 AP/mesh/双频同名时避免连到弱的那台）。
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

  err = esp_wifi_connect();
  if (err != ESP_OK) {
    connecting_ = false;
    ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
    diagResult(acc.ssid, WifiAttemptResult::Failed, 0);
    setState(WifiState::Failed);
    return state_;
  }

  EventBits_t bits = xEventGroupWaitBits(event_group_, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                         pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
  connecting_ = false;

  if (bits & WIFI_CONNECTED_BIT) {
    ESP_LOGI(TAG, "connected to %s", acc.ssid.c_str());
    diagResult(acc.ssid, WifiAttemptResult::Connected, 0);
    {
      MutexLock lock(diag_mutex_);
      fallback_policy_.onConnected();
    }
    applyPowerSave();
    return state_;
  }
  if (!(bits & WIFI_FAIL_BIT)) {
    esp_wifi_disconnect();  // 超时：主动断开停止底层尝试
    last_reason_ = 0;
    ESP_LOGW(TAG, "connect timeout to %s", acc.ssid.c_str());
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

  // 快连（电池定时唤醒）：一次固定 BSSID/信道的短超时尝试；失败回退常规路径。
  if (fast_hint_.valid) {
    const ApHint hint = fast_hint_;
    fast_hint_ = ApHint{};
    for (const WifiAccount& account : accounts) {
      if (account.ssid != hint.ssid) continue;
      fast_hint_used_ = true;
      ESP_LOGI(TAG, "fast connect %s (ch %u)", account.ssid.c_str(), (unsigned)hint.channel);
      if (connect(account, kFastConnectTimeoutMs, &hint) == WifiState::Connected) return state_;
      ESP_LOGW(TAG, "fast connect failed; falling back to scan");
      break;
    }
  }

  esp_wifi_disconnect();

  // 扫描只用于排序：可见的已知网络按列表顺序优先（各两次），未见网络（隐藏 SSID/
  // 弱信号漏检）随后各试一次；扫描失败则按列表顺序全部按名直连。
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
  size_t skip_index = SIZE_MAX;  // 驱动报告 AP 不存在 → 跳过同一网络的剩余尝试
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
  if (!radio.held) return false;  // net_task 正在连接
  if (ensureStarted() != ESP_OK) return false;
  if (state_ == WifiState::Connecting) return false;
  return scanLocked(nullptr);
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
