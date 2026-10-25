// SPDX-License-Identifier: GPL-3.0-or-later
#include "ble/ble_manager.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_id.h"
#include "host/ble_store.h"
#include "net_stack/credentials.h"
#include "net_stack/net_task.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "wifi_provisioning/scheme_ble.h"

extern "C" void ble_store_config_init(void);

namespace eego::ble {

namespace {

constexpr char kTag[] = "ble";
constexpr uint32_t kProvisioningWindowMs = 180000;
constexpr size_t kAdvQueueLength = 8;
constexpr size_t kProvQueueLength = 8;
constexpr size_t kMaxAdvBytes = 31;

int64_t nowMs() { return esp_timer_get_time() / 1000; }

void secureZero(void* data, size_t size) {
  volatile uint8_t* bytes = static_cast<volatile uint8_t*>(data);
  while (size-- > 0) *bytes++ = 0;
}

void secureZero(std::string& text) {
  if (!text.empty()) secureZero(text.data(), text.size());
  text.clear();
}

struct UnlockGuard {
  void* mutex;
  ~UnlockGuard() {
    if (mutex != nullptr) xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex));
  }
};

bool serviceDataUuid(const uint8_t* data, size_t length, size_t uuid_width,
                     std::string& canonical, std::vector<uint8_t>& payload) {
  if (data == nullptr || length < uuid_width || (uuid_width != 2 && uuid_width != 4 && uuid_width != 16)) {
    return false;
  }
  std::string hex;
  if (uuid_width == 16) {
    static constexpr char kHex[] = "0123456789abcdef";
    hex.reserve(32);
    for (size_t i = 0; i < 16; i++) {
      const uint8_t byte = data[15 - i];
      hex.push_back(kHex[byte >> 4]);
      hex.push_back(kHex[byte & 0x0f]);
    }
  } else {
    uint32_t value = 0;
    for (size_t i = 0; i < uuid_width; i++) value |= static_cast<uint32_t>(data[i]) << (i * 8);
    char buffer[9];
    std::snprintf(buffer, sizeof(buffer), uuid_width == 2 ? "%04lx" : "%08lx",
                  static_cast<unsigned long>(value));
    hex = buffer;
  }
  if (!eego::settings::normalizeBleUuid(hex, canonical)) return false;
  payload.assign(data + uuid_width, data + length);
  return true;
}

}  // namespace

struct BleManager::RawAdvertisement {
  uint8_t address[6]{};
  int8_t rssi = 0;
  uint8_t length = 0;
  uint8_t data[kMaxAdvBytes]{};
};

struct BleManager::ProvisionMessage {
  int event = -1;
  char ssid[33]{};
  char password[65]{};
};

BleManager& BleManager::instance() {
  static BleManager manager;
  return manager;
}

bool BleManager::compiled() {
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED && CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_1
  return true;
#else
  return false;
#endif
}

const char* roleName(Role role) {
  switch (role) {
    case Role::Observer: return "ble_scan";
    case Role::Provisioning: return "wifi_prov";
    default: return "off";
  }
}

const char* provisionStateName(ProvisionState state) {
  switch (state) {
    case ProvisionState::Active: return "active";
    case ProvisionState::Succeeded: return "succeeded";
    case ProvisionState::Failed: return "failed";
    case ProvisionState::TimedOut: return "timeout";
    case ProvisionState::Cancelled: return "cancelled";
    default: return "idle";
  }
}

void BleManager::lock() const {
  if (mutex_ != nullptr) xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
}

void BleManager::unlock() const {
  if (mutex_ != nullptr) xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
}

esp_err_t BleManager::init(CredentialSink sink, void* sink_ctx) {
  if (mutex_ == nullptr) {
    mutex_ = xSemaphoreCreateMutex();
    if (mutex_ == nullptr) return ESP_ERR_NO_MEM;
  }
  lock();
  UnlockGuard guard{mutex_};
  if (adv_queue_ == nullptr) adv_queue_ = xQueueCreate(kAdvQueueLength, sizeof(RawAdvertisement));
  if (prov_queue_ == nullptr) prov_queue_ = xQueueCreate(kProvQueueLength, sizeof(ProvisionMessage));
  if (adv_queue_ == nullptr || prov_queue_ == nullptr) return ESP_ERR_NO_MEM;
  credential_sink_ = sink;
  credential_sink_ctx_ = sink_ctx;
  updateMemoryStatusLocked();
  return ESP_OK;
}

void BleManager::configure(const eego::settings::Settings& settings) {
  lock();
  UnlockGuard guard{mutex_};
  if (role_.load() == Role::Observer) stopObserverLocked();
  settings_ = settings;
  observer_.configure(settings_.ble_observer_rules, settings_.ble_observer_dedup_ms);
  if (adv_queue_ != nullptr) xQueueReset(static_cast<QueueHandle_t>(adv_queue_));
  battery_scan_done_ = false;
  next_observer_ms_ = nowMs();
}

void BleManager::onPowerMode(bool mains) {
  lock();
  UnlockGuard guard{mutex_};
  if (mains_ == mains) return;
  if (role_.load() == Role::Observer) stopObserverLocked();
  mains_ = mains;
  battery_window_open_ = false;
  battery_scan_done_ = false;
  next_observer_ms_ = nowMs();
}

void BleManager::setBatteryScanWindow(bool open) {
  lock();
  UnlockGuard guard{mutex_};
  if (mains_ || battery_window_open_ == open) return;
  battery_window_open_ = open;
  if (open) {
    battery_scan_done_ = false;
    next_observer_ms_ = nowMs();
  } else if (role_.load() == Role::Observer) {
    stopObserverLocked();
  }
}

esp_err_t BleManager::startObserverLocked() {
  if (role_.load() != Role::Off) return ESP_ERR_INVALID_STATE;
  if (!settings_.ble_observer_enabled || settings_.ble_observer_rules.empty()) {
    return ESP_ERR_INVALID_STATE;
  }
  role_ = Role::Observer;
  observer_synced_ = false;
  observer_disc_complete_ = false;
  observer_failed_ = false;
  observer_stop_pending_ = false;
  last_error_.clear();
  const int64_t now = nowMs();
  observer_started_ms_ = now;
  observer_end_ms_ = now + static_cast<int64_t>(settings_.ble_observer_window_s) * 1000 + 3000;
  updateMemoryStatusLocked();
  const esp_err_t err = nimble_port_init();
  if (err != ESP_OK) {
    role_ = Role::Off;
    observer_started_ms_ = 0;
    next_observer_ms_ = now + 5000;
    setErrorLocked("NimBLE init failed");
    return err;
  }
  ble_hs_cfg.reset_cb = &BleManager::onReset;
  ble_hs_cfg.sync_cb = &BleManager::onSync;
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
  ble_hs_cfg.sm_bonding = 0;
  ble_hs_cfg.sm_mitm = 0;
  ble_hs_cfg.sm_sc = 0;
  ble_store_config_init();
  nimble_port_freertos_init(&BleManager::hostTask);
  ESP_LOGI(kTag, "observer window start (%lus)", static_cast<unsigned long>(settings_.ble_observer_window_s));
  return ESP_OK;
}

bool BleManager::stopObserverLocked() {
  if (role_.load() != Role::Observer) return true;
  observer_stop_pending_ = true;
  const int64_t now = nowMs();
  if (now < stop_retry_ms_) return false;
  if (observer_started_ms_ >= 0) {
    if (observer_synced_.load()) ble_gap_disc_cancel();
    const int rc = nimble_port_stop();
    if (rc != 0) {
      stop_retry_ms_ = now + 200;
      setErrorLocked("NimBLE host stop failed; retrying");
      ESP_LOGW(kTag, "observer host stop rc=%d", rc);
      return false;
    }
    observer_started_ms_ = -1;
  }
  const esp_err_t err = nimble_port_deinit();
  if (err != ESP_OK) {
    stop_retry_ms_ = now + 200;
    setErrorLocked("NimBLE deinit failed; retrying");
    ESP_LOGW(kTag, "observer deinit err=%s", esp_err_to_name(err));
    return false;
  }
  role_ = Role::Off;
  observer_synced_ = false;
  observer_stop_pending_ = false;
  observer_disc_complete_ = false;
  observer_failed_ = false;
  observer_started_ms_ = 0;
  stop_retry_ms_ = 0;
  next_observer_ms_ = now + static_cast<int64_t>(settings_.ble_observer_interval_s) * 1000;
  if (!mains_) battery_scan_done_ = true;
  updateMemoryStatusLocked();
  ESP_LOGI(kTag, "observer stopped and released");
  return true;
}

void BleManager::hostTask(void* arg) {
  (void)arg;
  nimble_port_run();
  nimble_port_freertos_deinit();
}

void BleManager::onSync() {
  BleManager& self = instance();
  if (self.role_.load() != Role::Observer || self.observer_stop_pending_) return;
  self.observer_synced_ = true;
  uint8_t own_addr_type = BLE_ADDR_PUBLIC;
  int rc = ble_hs_id_infer_auto(0, &own_addr_type);
  if (rc == 0) {
    ble_gap_disc_params params = {};
    params.itvl = 0x50;
    params.window = 0x30;
    params.passive = 1;
    params.filter_duplicates = 0;
    rc = ble_gap_disc(own_addr_type,
                      static_cast<int32_t>(self.settings_.ble_observer_window_s * 1000),
                      &params, &BleManager::onGapEvent, &self);
  }
  if (rc != 0) {
    self.observer_failed_ = true;
    ESP_LOGW(kTag, "observer scan start rc=%d", rc);
  }
}

void BleManager::onReset(int reason) {
  if (instance().role_.load() == Role::Observer) instance().observer_failed_ = true;
  ESP_LOGW(kTag, "NimBLE reset reason=%d", reason);
}

int BleManager::onGapEvent(struct ble_gap_event* event, void* arg) {
  BleManager* self = static_cast<BleManager*>(arg);
  if (self == nullptr || event == nullptr || self->role_.load() != Role::Observer) return 0;
  if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
    self->observer_disc_complete_ = true;
    return 0;
  }
  if (event->type != BLE_GAP_EVENT_DISC || event->disc.data == nullptr ||
      event->disc.length_data == 0 || event->disc.length_data > kMaxAdvBytes ||
      self->adv_queue_ == nullptr) {
    if (event->type == BLE_GAP_EVENT_DISC) self->dropped_advertisements_++;
    return 0;
  }
  RawAdvertisement raw;
  std::memcpy(raw.address, event->disc.addr.val, sizeof(raw.address));
  raw.rssi = event->disc.rssi;
  raw.length = event->disc.length_data;
  std::memcpy(raw.data, event->disc.data, raw.length);
  if (xQueueSend(static_cast<QueueHandle_t>(self->adv_queue_), &raw, 0) != pdTRUE) {
    self->dropped_advertisements_++;
  }
  return 0;
}

void BleManager::drainAdvertisementsLocked() {
  if (adv_queue_ == nullptr) return;
  for (size_t packet = 0; packet < 4; packet++) {
    RawAdvertisement raw;
    if (xQueueReceive(static_cast<QueueHandle_t>(adv_queue_), &raw, 0) != pdTRUE) return;
    ble_hs_adv_fields fields = {};
    if (ble_hs_adv_parse_fields(&fields, raw.data, raw.length) != 0) {
      malformed_advertisements_++;
      continue;
    }
    Advertisement base;
    std::copy(std::begin(raw.address), std::end(raw.address), base.address.begin());
    base.rssi = raw.rssi;
    if (fields.mfg_data != nullptr && fields.mfg_data_len >= 2) {
      base.has_manufacturer = true;
      base.manufacturer_id = static_cast<uint16_t>(fields.mfg_data[0]) |
                             (static_cast<uint16_t>(fields.mfg_data[1]) << 8);
      base.manufacturer_payload.assign(fields.mfg_data + 2, fields.mfg_data + fields.mfg_data_len);
    }
    const uint8_t* service_data[3] = {fields.svc_data_uuid16, fields.svc_data_uuid32,
                                      fields.svc_data_uuid128};
    const uint8_t service_lengths[3] = {fields.svc_data_uuid16_len, fields.svc_data_uuid32_len,
                                        fields.svc_data_uuid128_len};
    const size_t widths[3] = {2, 4, 16};
    size_t ingested = 0;
    auto process = [&](Advertisement& advertisement) {
      if (ingested >= 8) return;
      std::vector<Observation> observations;
      const auto result = observer_.process(advertisement, nowMs(), observations);
      duplicate_advertisements_ += result.duplicates;
      malformed_advertisements_ += result.malformed;
      for (const auto& observation : observations) {
        if (ingested >= 8) break;
        std::vector<alert_core::AlertEvent> events{observation.event};
        if (eego::net::NetStack::instance().ingestExternal(events, observation.source, 25)) {
          emitted_events_++;
        } else {
          ingest_errors_++;
        }
        ingested++;
      }
    };
    if (base.has_manufacturer) process(base);
    for (size_t i = 0; i < 3; i++) {
      if (service_data[i] == nullptr || service_lengths[i] < widths[i]) continue;
      Advertisement advertisement;
      advertisement.address = base.address;
      advertisement.rssi = base.rssi;
      if (!serviceDataUuid(service_data[i], service_lengths[i], widths[i],
                           advertisement.service_uuid, advertisement.service_payload)) {
        malformed_advertisements_++;
        continue;
      }
      advertisement.has_service_data = true;
      process(advertisement);
    }
  }
}

esp_err_t BleManager::startProvisioningFromLocal(const std::string& pop) {
  lock();
  UnlockGuard guard{mutex_};
  if (!compiled()) return ESP_ERR_NOT_SUPPORTED;
  if (recovery_ap_active_.load() || role_.load() == Role::Provisioning) return ESP_ERR_INVALID_STATE;
  if (pop.size() < 8 || pop.size() > 32) return ESP_ERR_INVALID_ARG;
  for (unsigned char c : pop) {
    if (c < 0x20 || c > 0x7e) return ESP_ERR_INVALID_ARG;
  }
  if (role_.load() == Role::Observer && !stopObserverLocked()) return ESP_ERR_INVALID_STATE;
  if (prov_queue_ == nullptr || credential_sink_ == nullptr) return ESP_ERR_INVALID_STATE;

  xQueueReset(static_cast<QueueHandle_t>(prov_queue_));
  provisioning_credentials_saved_ = false;
  provisioning_credentials_failed_ = false;
  provisioning_transport_fault_ = false;
  provisioning_success_ = false;
  provisioning_end_ = false;
  provision_state_ = ProvisionState::Active;
  last_error_.clear();
  pop_ = pop;
  uint8_t mac[6] = {};
  if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
    char name[20];
    std::snprintf(name, sizeof(name), "EEGO-A4-%02X%02X", mac[4], mac[5]);
    service_name_ = name;
  } else {
    service_name_ = "EEGO-A4-PROV";
  }
  role_ = Role::Provisioning;
  eego::net::NetStack::instance().setWifiProvisioningHold(true);
  updateMemoryStatusLocked();

  wifi_prov_mgr_config_t config = {};
  config.scheme = wifi_prov_scheme_ble;
  config.app_event_handler.event_cb = &BleManager::onProvisioningEvent;
  config.app_event_handler.user_data = this;
  config.wifi_prov_conn_cfg.wifi_conn_attempts = 2;
  esp_err_t err = wifi_prov_mgr_init(config);
  const bool manager_initialized = err == ESP_OK;
  if (err == ESP_OK) err = wifi_prov_mgr_disable_auto_stop(1000);
  if (err == ESP_OK) {
    err = wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1, pop_.c_str(),
                                           service_name_.c_str(), nullptr);
  }
  if (err != ESP_OK) {
    if (manager_initialized) wifi_prov_mgr_deinit();
    eego::net::NetStack::instance().setWifiProvisioningHold(false);
    role_ = Role::Off;
    provision_state_ = ProvisionState::Failed;
    setErrorLocked("BLE provisioning start failed");
    secureZero(pop_);
    return err;
  }
  provisioning_deadline_ms_ = nowMs() + kProvisioningWindowMs;
  provisioning_success_ms_ = 0;
  ESP_LOGI(kTag, "local secure provisioning window started");
  return ESP_OK;
}

void BleManager::onProvisioningEvent(void* arg, wifi_prov_cb_event_t event, void* event_data) {
  BleManager* self = static_cast<BleManager*>(arg);
  if (self == nullptr || self->prov_queue_ == nullptr) return;
  ProvisionMessage message;
  message.event = static_cast<int>(event);
  if (event == WIFI_PROV_CRED_RECV) {
    if (event_data == nullptr) {
      self->provisioning_transport_fault_ = true;
      return;
    }
    const wifi_sta_config_t* config = static_cast<const wifi_sta_config_t*>(event_data);
    const char* ssid = reinterpret_cast<const char*>(config->ssid);
    const char* password = reinterpret_cast<const char*>(config->password);
    const size_t ssid_len = strnlen(ssid, sizeof(config->ssid));
    const size_t password_len = strnlen(password, sizeof(config->password));
    if (ssid_len == 0) {
      self->provisioning_transport_fault_ = true;
      return;
    }
    std::memcpy(message.ssid, ssid, ssid_len);
    std::memcpy(message.password, password, password_len);
  }
  if (xQueueSend(static_cast<QueueHandle_t>(self->prov_queue_), &message, 0) != pdTRUE) {
    secureZero(&message, sizeof(message));
    self->provisioning_transport_fault_ = true;
  }
  secureZero(&message, sizeof(message));
}

void BleManager::drainProvisioningLocked() {
  if (prov_queue_ == nullptr || role_.load() != Role::Provisioning) return;
  for (size_t i = 0; i < kProvQueueLength; i++) {
    ProvisionMessage message;
    if (xQueueReceive(static_cast<QueueHandle_t>(prov_queue_), &message, 0) != pdTRUE) break;
    const int event = message.event;
    if (event == WIFI_PROV_CRED_RECV) {
      const esp_err_t err = credential_sink_(message.ssid, message.password, credential_sink_ctx_);
      if (err == ESP_OK) {
        provisioning_credentials_saved_ = true;
      } else {
        provisioning_credentials_failed_ = true;
        provision_state_ = ProvisionState::Failed;
        setErrorLocked("WiFi credential NVS commit failed");
      }
    } else if (event == WIFI_PROV_CRED_SUCCESS) {
      if (provisioning_credentials_saved_ && !provisioning_credentials_failed_) {
        provisioning_success_ = true;
        provisioning_success_ms_ = nowMs();
        provision_state_ = ProvisionState::Succeeded;
        wifi_prov_mgr_stop_provisioning();
      } else {
        provisioning_credentials_failed_ = true;
        provision_state_ = ProvisionState::Failed;
      }
    } else if (event == WIFI_PROV_CRED_FAIL) {
      provisioning_credentials_failed_ = true;
      provision_state_ = ProvisionState::Failed;
      setErrorLocked("WiFi connection during BLE provisioning failed");
    } else if (event == WIFI_PROV_END) {
      provisioning_end_ = true;
    }
    secureZero(&message, sizeof(message));
    if (provisioning_credentials_failed_) {
      stopProvisioningLocked(ProvisionState::Failed);
      return;
    }
  }
  if (provisioning_transport_fault_.exchange(false)) {
    provision_state_ = ProvisionState::Failed;
    setErrorLocked("provisioning event queue unavailable");
    stopProvisioningLocked(ProvisionState::Failed);
  }
}

void BleManager::stopProvisioningLocked(ProvisionState terminal) {
  if (role_.load() != Role::Provisioning) return;
  wifi_prov_mgr_deinit();
  eego::net::NetStack::instance().setWifiProvisioningHold(false);
  role_ = Role::Off;
  provision_state_ = terminal;
  provisioning_deadline_ms_ = 0;
  provisioning_success_ms_ = 0;
  provisioning_success_ = false;
  provisioning_end_ = false;
  if (prov_queue_ != nullptr) xQueueReset(static_cast<QueueHandle_t>(prov_queue_));
  secureZero(pop_);
  service_name_.clear();
  next_observer_ms_ = nowMs() + 2000;
  updateMemoryStatusLocked();
  ESP_LOGI(kTag, "provisioning window closed (%s)", provisionStateName(terminal));
}

void BleManager::cancelProvisioning() {
  lock();
  UnlockGuard guard{mutex_};
  stopProvisioningLocked(ProvisionState::Cancelled);
}

bool BleManager::provisioningActive() const { return role_.load() == Role::Provisioning; }

bool BleManager::sleepBlocked() const { return role_.load() != Role::Off; }

esp_err_t BleManager::beforeSleep() {
  lock();
  UnlockGuard guard{mutex_};
  stopProvisioningLocked(ProvisionState::Cancelled);
  if (!stopObserverLocked()) return ESP_ERR_INVALID_STATE;
  if (adv_queue_ != nullptr) xQueueReset(static_cast<QueueHandle_t>(adv_queue_));
  battery_window_open_ = false;
  battery_scan_done_ = false;
  return role_.load() == Role::Off ? ESP_OK : ESP_ERR_INVALID_STATE;
}

bool BleManager::beginRecoveryAp() {
  lock();
  UnlockGuard guard{mutex_};
  if (role_.load() == Role::Provisioning) return false;
  if (role_.load() == Role::Observer && !stopObserverLocked()) return false;
  recovery_ap_active_ = true;
  return true;
}

void BleManager::endRecoveryAp() { recovery_ap_active_ = false; }

esp_err_t BleManager::clearBonds() {
  lock();
  UnlockGuard guard{mutex_};
  stopProvisioningLocked(ProvisionState::Cancelled);
  if (!stopObserverLocked() || role_.load() != Role::Off) return ESP_ERR_INVALID_STATE;
  const esp_err_t err = eego::net::CredentialStore::clearNamespace("nimble_bond");
  if (err != ESP_OK) setErrorLocked("BLE bond NVS erase failed");
  next_observer_ms_ = nowMs() + 1000;
  return err;
}

void BleManager::service(bool mains) {
  lock();
  UnlockGuard guard{mutex_};
  const int64_t now = nowMs();
  if (mains_ != mains) {
    if (role_.load() == Role::Observer) stopObserverLocked();
    mains_ = mains;
    battery_window_open_ = false;
    battery_scan_done_ = false;
    next_observer_ms_ = now;
  }
  if (now - memory_sample_ms_ >= 1000) {
    memory_sample_ms_ = now;
    updateMemoryStatusLocked();
  }
  drainProvisioningLocked();
  drainAdvertisementsLocked();

  if (role_.load() == Role::Provisioning) {
    if (provisioning_end_ || (provisioning_success_ && now - provisioning_success_ms_ > 10000)) {
      stopProvisioningLocked(provision_state_);
    } else if (now >= provisioning_deadline_ms_) {
      stopProvisioningLocked(ProvisionState::TimedOut);
    }
    return;
  }
  if (role_.load() == Role::Observer) {
    if (observer_stop_pending_) {
      stopObserverLocked();
    } else if (observer_failed_.exchange(false)) {
      setErrorLocked("observer scan failed");
      stopObserverLocked();
    } else if (observer_disc_complete_.load() || now >= observer_end_ms_ || recovery_ap_active_.load()) {
      stopObserverLocked();
    }
    return;
  }
  if (!settings_.ble_observer_enabled || settings_.ble_observer_rules.empty() ||
      recovery_ap_active_.load() || now < next_observer_ms_) {
    return;
  }
  if (!mains_ && (!battery_window_open_ || battery_scan_done_)) return;
  startObserverLocked();
}

void BleManager::setErrorLocked(const std::string& error) {
  last_error_ = error;
  ESP_LOGW(kTag, "%s", error.c_str());
}

void BleManager::updateMemoryStatusLocked() {
  diram_free_bytes_ = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  diram_min_free_bytes_ = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  psram_free_bytes_ = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

Status BleManager::status() const {
  lock();
  UnlockGuard guard{mutex_};
  Status out;
  out.compiled = compiled();
  const Role role = role_.load();
  out.stack_active = role != Role::Off;
  out.recovery_ap_active = recovery_ap_active_.load();
  out.role = role;
  out.provisioning = provision_state_;
  const int64_t now = nowMs();
  if (role == Role::Provisioning && provisioning_deadline_ms_ > now) {
    out.provisioning_remaining_s = (provisioning_deadline_ms_ - now + 999) / 1000;
  }
  if (role == Role::Observer && observer_end_ms_ > now) {
    out.observer_remaining_s = (observer_end_ms_ - now + 999) / 1000;
  }
  out.dropped_advertisements = dropped_advertisements_.load();
  out.emitted_events = emitted_events_;
  out.duplicate_advertisements = duplicate_advertisements_;
  out.malformed_advertisements = malformed_advertisements_;
  out.ingest_errors = ingest_errors_;
  out.diram_free_bytes = diram_free_bytes_;
  out.diram_min_free_bytes = diram_min_free_bytes_;
  out.psram_free_bytes = psram_free_bytes_;
  out.last_error = last_error_;
  return out;
}

}  // namespace eego::ble
