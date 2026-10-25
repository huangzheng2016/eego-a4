// SPDX-License-Identifier: GPL-3.0-or-later
// WiFi STA 状态机实现（改造自 eego-a4-weread wifi_manager.cpp，GPL-3.0）。
#include "net_stack/wifi_manager.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"

namespace eego::net {

static const char* TAG = "net.wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WIFI_MAX_RETRIES 3  // 每个 SSID 最多重试次数
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

WifiManager& WifiManager::instance() {
  static WifiManager inst;
  return inst;
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
  err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandler, this);
  if (err != ESP_OK) return err;
  err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifiEventHandler, this);
  if (err != ESP_OK) return err;

  err = esp_wifi_set_mode(WIFI_MODE_STA);
  if (err != ESP_OK) return err;
  err = esp_wifi_start();
  if (err != ESP_OK) return err;

  applyPowerSave();
  // TX 功率由 sdkconfig CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=20 控制，无需运行时设置
  // （esp_wifi_set_max_tx_power 单位是 0.25dBm，误传 20 会变 5dBm，weread 教训）。

  event_group_ = xEventGroupCreate();
  if (!event_group_) return ESP_ERR_NO_MEM;

  initialized_ = true;
  ESP_LOGI(TAG, "wifi initialized (STA mode)");
  return ESP_OK;
}

void WifiManager::setPowerSave(bool enable) {
  ps_enabled_ = enable;
  if (initialized_) applyPowerSave();
}

void WifiManager::applyPowerSave() {
  // weread 实证：esp_wifi_start() 会把省电重置为默认 MIN_MODEM，
  // 每次 start/reconnect 后必须重新应用。
  esp_wifi_set_ps(ps_enabled_ ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
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
        ESP_LOGW(TAG, "STA disconnected, reason=%d", (int)evt->reason);
        self->last_reason_ = evt->reason;
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
  if (!initialized_ && init() != ESP_OK) {
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

  wifi_config_t cfg = {};
  strncpy((char*)cfg.sta.ssid, acc.ssid.c_str(), sizeof(cfg.sta.ssid) - 1);
  strncpy((char*)cfg.sta.password, acc.password.c_str(), sizeof(cfg.sta.password) - 1);
  // 认证阈值 WPA_PSK：接受 WPA/WPA2/WPA3 常用加密（阈值不是开关，不能设 OPEN）
  cfg.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
  if (fast) {
    // 固定 BSSID + 信道：驱动只在该信道探测该 AP，跳过全信道扫描。
    cfg.sta.bssid_set = true;
    memcpy(cfg.sta.bssid, hint->bssid, sizeof(cfg.sta.bssid));
    cfg.sta.channel = hint->channel;
    cfg.sta.scan_method = WIFI_FAST_SCAN;
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

  err = esp_wifi_connect();
  if (err != ESP_OK) {
    connecting_ = false;
    ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
    setState(WifiState::Failed);
    return state_;
  }

  EventBits_t bits = xEventGroupWaitBits(event_group_, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                         pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
  connecting_ = false;

  if (bits & WIFI_CONNECTED_BIT) {
    ESP_LOGI(TAG, "connected to %s", acc.ssid.c_str());
    return state_;
  }
  if (!(bits & WIFI_FAIL_BIT)) {
    esp_wifi_disconnect();  // 超时：主动断开停止底层尝试
    last_reason_ = 0;
    ESP_LOGW(TAG, "connect timeout to %s", acc.ssid.c_str());
  } else {
    ESP_LOGW(TAG, "connect failed to %s, reason=%d", acc.ssid.c_str(), (int)last_reason_);
  }
  setState(WifiState::Failed);
  return state_;
}

WifiState WifiManager::connectMulti(const std::vector<WifiAccount>& accounts) {
  if (!initialized_ && init() != ESP_OK) {
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
      ESP_LOGI(TAG, "fast connect %s (ch %u)", account.ssid.c_str(), (unsigned)hint.channel);
      if (connect(account, kFastConnectTimeoutMs, &hint) == WifiState::Connected) return state_;
      ESP_LOGW(TAG, "fast connect failed; falling back to scan");
      break;
    }
  }

  esp_wifi_disconnect();

  // 扫描仅作诊断提示（弱信号/隐藏网络常漏检），按名直连更可靠
  wifi_scan_config_t scan_cfg = {};
  scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  scan_cfg.show_hidden = true;
  bool scan_ok = esp_wifi_scan_start(&scan_cfg, true) == ESP_OK;
  uint16_t ap_num = 0;
  wifi_ap_record_t* aps = nullptr;
  if (scan_ok && esp_wifi_scan_get_ap_num(&ap_num) == ESP_OK && ap_num > 0) {
    aps = (wifi_ap_record_t*)malloc(sizeof(wifi_ap_record_t) * ap_num);
    if (aps && esp_wifi_scan_get_ap_records(&ap_num, aps) != ESP_OK) {
      free(aps);
      aps = nullptr;
      ap_num = 0;
    }
  }
  if (aps) {
    ESP_LOGI(TAG, "scan found %u APs", (unsigned)ap_num);
    free(aps);
  }

  bool any_success = false;
  for (size_t i = 0; i < accounts.size() && !any_success; i++) {
    if (accounts[i].ssid.empty()) continue;
    for (int retry = 0; retry < WIFI_MAX_RETRIES && !any_success; retry++) {
      uint32_t tmo = (retry == 0) ? 30000 : 15000;
      ESP_LOGI(TAG, "connecting %s (try %d/%d, timeout=%lums)", accounts[i].ssid.c_str(),
               retry + 1, WIFI_MAX_RETRIES, (unsigned long)tmo);
      if (connect(accounts[i], tmo) == WifiState::Connected) any_success = true;
    }
  }

  if (!any_success) {
    ESP_LOGW(TAG, "all wifi accounts failed, offline mode");
    setState(WifiState::OfflineMode);
  }
  return state_;
}

void WifiManager::powerOff() {
  connecting_ = false;
  if (initialized_) {
    esp_wifi_disconnect();
    esp_wifi_stop();
  }
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
