// SPDX-License-Identifier: GPL-3.0-or-later
#include "ble/ble_manager.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_id.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "net_stack/credentials.h"
#include "net_stack/net_task.h"
#include "net_stack/wifi_manager.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

extern "C" void ble_store_config_init(void);

namespace eego::ble {

namespace {

constexpr char kTag[] = "ble";
constexpr size_t kAdvQueueLength = 8;
constexpr size_t kMaxAdvBytes = 31;
constexpr int64_t kPasskeyWindowMs = 30000;      // SMP 超时
constexpr int64_t kPairFailWindowMs = 300000;    // 5 分钟内
constexpr uint32_t kPairFailLimit = 3;           // 失败 3 次
constexpr int64_t kPairLockMs = 300000;          // 锁定 5 分钟
constexpr int64_t kTunnelQuietMs = 30000;        // 隧道活跃期间不开监听窗口
constexpr size_t kMaxFrameBytes = 516;

// e3e0a400-7c1b-4f7e-9d2a-5ee90a4b00xx（小端字节序）
#define EEGO_TUNNEL_UUID(lo) \
  BLE_UUID128_INIT(lo, 0x00, 0x4b, 0x0a, 0xe9, 0x5e, 0x2a, 0x9d, 0x7e, 0x4f, 0x1b, 0x7c, 0x00, 0xa4, 0xe0, 0xe3)

const ble_uuid128_t kSvcUuid = EEGO_TUNNEL_UUID(0x00);
const ble_uuid128_t kInfoUuid = EEGO_TUNNEL_UUID(0x01);
const ble_uuid128_t kRxUuid = EEGO_TUNNEL_UUID(0x02);
const ble_uuid128_t kTxUuid = EEGO_TUNNEL_UUID(0x03);

uint16_t g_info_handle = 0;
uint16_t g_rx_handle = 0;
uint16_t g_tx_handle = 0;
uint8_t g_own_addr_type = BLE_OWN_ADDR_PUBLIC;
SemaphoreHandle_t g_send_mutex = nullptr;

const ble_gatt_chr_def kChrs[] = {
    {
        .uuid = &kInfoUuid.u,
        .access_cb = &BleManager::onGattAccess,
        .arg = nullptr,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_READ_AUTHEN,
        .min_key_size = 16,
        .val_handle = &g_info_handle,
        .cpfd = nullptr,
    },
    {
        .uuid = &kRxUuid.u,
        .access_cb = &BleManager::onGattAccess,
        .arg = nullptr,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC |
                 BLE_GATT_CHR_F_WRITE_AUTHEN,
        .min_key_size = 16,
        .val_handle = &g_rx_handle,
        .cpfd = nullptr,
    },
    {
        .uuid = &kTxUuid.u,
        .access_cb = &BleManager::onGattAccess,
        .arg = nullptr,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                 BLE_GATT_CHR_F_READ_AUTHEN,
        .min_key_size = 16,
        .val_handle = &g_tx_handle,
        .cpfd = nullptr,
    },
    {},
};

const ble_gatt_svc_def kSvcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &kSvcUuid.u,
        .includes = nullptr,
        .characteristics = kChrs,
    },
    {},
};

int64_t nowMs() { return esp_timer_get_time() / 1000; }

uint8_t* psramAlloc(size_t bytes) {
  void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p == nullptr) p = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
  return static_cast<uint8_t*>(p);
}

void psramFree(uint8_t* ptr) { heap_caps_free(ptr); }

struct UnlockGuard {
  void* mutex;
  ~UnlockGuard() {
    if (mutex != nullptr) xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex));
  }
};

struct RxLock {
  void* mutex;
  explicit RxLock(void* m) : mutex(m) {
    if (mutex != nullptr) xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex), portMAX_DELAY);
  }
  ~RxLock() {
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

void jsonEscapeInto(std::string& out, const std::string& in) {
  for (char c : in) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
    } else if (static_cast<unsigned char>(c) >= 0x20) {
      out.push_back(c);
    }
  }
}

}  // namespace

struct BleManager::RawAdvertisement {
  uint8_t address[6]{};
  int8_t rssi = 0;
  uint8_t length = 0;
  uint8_t data[kMaxAdvBytes]{};
};

BleManager& BleManager::instance() {
  static BleManager manager;
  return manager;
}

bool BleManager::compiled() {
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
  return true;
#else
  return false;
#endif
}

const char* roleName(Role role) {
  switch (role) {
    case Role::Observer: return "ble_scan";
    case Role::Admin: return "ble_admin";
    default: return "off";
  }
}

const char* linkStateName(LinkState state) {
  switch (state) {
    case LinkState::Connected: return "connected";
    case LinkState::Pairing: return "pairing";
    case LinkState::Authenticated: return "authenticated";
    default: return "none";
  }
}

void BleManager::lock() const {
  if (mutex_ != nullptr) xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
}

void BleManager::unlock() const {
  if (mutex_ != nullptr) xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
}

esp_err_t BleManager::init() {
  if (mutex_ == nullptr) {
    mutex_ = xSemaphoreCreateMutex();
    if (mutex_ == nullptr) return ESP_ERR_NO_MEM;
  }
  if (rx_mutex_ == nullptr) rx_mutex_ = xSemaphoreCreateMutex();
  if (tx_sem_ == nullptr) tx_sem_ = xSemaphoreCreateBinary();
  if (g_send_mutex == nullptr) g_send_mutex = xSemaphoreCreateMutex();
  lock();
  UnlockGuard guard{mutex_};
  if (adv_queue_ == nullptr) adv_queue_ = xQueueCreate(kAdvQueueLength, sizeof(RawAdvertisement));
  if (adv_queue_ == nullptr || rx_mutex_ == nullptr || tx_sem_ == nullptr || g_send_mutex == nullptr) {
    return ESP_ERR_NO_MEM;
  }
  if (device_name_.empty()) {
    uint8_t mac[6] = {};
    char name[20];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
      std::snprintf(name, sizeof(name), "EEGO-A4-%02X%02X", mac[4], mac[5]);
    } else {
      std::snprintf(name, sizeof(name), "EEGO-A4");
    }
    device_name_ = name;
  }
  updateMemoryStatusLocked();
  return ESP_OK;
}

void BleManager::configure(const eego::settings::Settings& settings) {
  lock();
  UnlockGuard guard{mutex_};
  if (scanning_.load()) stopObserverWindowLocked();
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
  if (scanning_.load()) stopObserverWindowLocked();
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
  } else if (scanning_.load()) {
    stopObserverWindowLocked();
  }
}

void BleManager::setAdminAllowed(bool allowed) { admin_allowed_.store(allowed && compiled()); }

void BleManager::setTunnelHandler(const TunnelHandler& handler) {
  RxLock rx(rx_mutex_);
  handler_ = handler;
}

void BleManager::setTunnelInfo(const std::string& fw, bool provisioned, bool httpd) {
  {
    RxLock rx(rx_mutex_);
    info_fw_ = fw;
  }
  info_provisioned_.store(provisioned);
  info_httpd_.store(httpd);
}

std::string BleManager::infoJson() const {
  std::string fw;
  {
    RxLock rx(rx_mutex_);
    fw = info_fw_;
  }
  char buf[96];
  std::string out = "{\"v\":1,\"mtu\":";
  std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(att_mtu_.load()));
  out += buf;
  out += ",\"name\":\"";
  jsonEscapeInto(out, device_name_);
  out += "\",\"fw\":\"";
  jsonEscapeInto(out, fw);
  out += "\",\"provisioned\":";
  out += info_provisioned_.load() ? "true" : "false";
  out += ",\"httpd\":";
  out += info_httpd_.load() ? "true" : "false";
  std::snprintf(buf, sizeof(buf), ",\"max_req\":%u,\"max_resp\":%u}",
                static_cast<unsigned>(tunnel::kMaxRequestBytes),
                static_cast<unsigned>(tunnel::kMaxResponseBytes));
  out += buf;
  return out;
}

// ---------------------------------------------------------------------------
// 主机生命周期
// ---------------------------------------------------------------------------

bool BleManager::observerDueLocked(int64_t now) const {
  if (!settings_.ble_observer_enabled || settings_.ble_observer_rules.empty()) return false;
  if (recovery_ap_active_.load() || now < next_observer_ms_) return false;
  if (now < observer_defer_until_ms_.load()) return false;
  if (link_.load() != LinkState::None && now - last_tunnel_ms_.load() < kTunnelQuietMs) return false;
  // WiFi 正在关联/握手：推迟 BLE 扫描窗口，避免共存时隙争用导致连接失败。
  if (eego::net::WifiManager::instance().state() == eego::net::WifiState::Connecting) return false;
  if (!mains_ && (!battery_window_open_ || battery_scan_done_)) return false;
  return true;
}

bool BleManager::wantHostLocked(int64_t now) const {
  if (!compiled()) return false;
  return admin_allowed_.load() || scanning_.load() || observerDueLocked(now);
}

esp_err_t BleManager::startHostLocked() {
  if (role_.load() != Role::Off) return ESP_ERR_INVALID_STATE;
  host_synced_ = false;
  host_failed_ = false;
  host_stop_pending_ = false;
  scanning_ = false;
  scan_complete_ = false;
  advertising_ = false;
  last_error_.clear();
  const int64_t now = nowMs();
  updateMemoryStatusLocked();
  const esp_err_t err = nimble_port_init();
  if (err != ESP_OK) {
    next_observer_ms_ = now + 5000;
    setErrorLocked("NimBLE init failed");
    return err;
  }
  ble_hs_cfg.reset_cb = &BleManager::onReset;
  ble_hs_cfg.sync_cb = &BleManager::onSync;
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
  ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;
  ble_hs_cfg.sm_bonding = 1;
  ble_hs_cfg.sm_mitm = 1;
  ble_hs_cfg.sm_sc = 1;
  ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

  ble_svc_gap_init();
  ble_svc_gatt_init();
  int rc = ble_gatts_count_cfg(kSvcs);
  if (rc == 0) rc = ble_gatts_add_svcs(kSvcs);
  if (rc != 0) {
    ESP_LOGE(kTag, "GATT service register rc=%d", rc);
    nimble_port_deinit();
    next_observer_ms_ = now + 5000;
    setErrorLocked("GATT service register failed");
    return ESP_FAIL;
  }
  ble_svc_gap_device_name_set(device_name_.c_str());
  ble_store_config_init();
  role_ = admin_allowed_.load() ? Role::Admin : Role::Observer;
  host_started_ms_ = now;
  nimble_port_freertos_init(&BleManager::hostTask);
  ESP_LOGI(kTag, "host start (%s)", roleName(role_.load()));
  return ESP_OK;
}

bool BleManager::stopHostLocked() {
  if (role_.load() == Role::Off) return true;
  host_stop_pending_ = true;
  const int64_t now = nowMs();
  if (now < stop_retry_ms_) return false;
  if (host_started_ms_ >= 0) {
    if (host_synced_.load()) {
      if (scanning_.load()) ble_gap_disc_cancel();
      if (ble_gap_adv_active()) ble_gap_adv_stop();
      const uint16_t conn = conn_handle_.load();
      if (conn != 0xffff) ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    const int rc = nimble_port_stop();
    if (rc != 0) {
      stop_retry_ms_ = now + 200;
      setErrorLocked("NimBLE host stop failed; retrying");
      ESP_LOGW(kTag, "host stop rc=%d", rc);
      return false;
    }
    host_started_ms_ = -1;
  }
  const esp_err_t err = nimble_port_deinit();
  if (err != ESP_OK) {
    stop_retry_ms_ = now + 200;
    setErrorLocked("NimBLE deinit failed; retrying");
    ESP_LOGW(kTag, "host deinit err=%s", esp_err_to_name(err));
    return false;
  }
  if (scanning_.load()) {
    scanning_ = false;
    next_observer_ms_ = now + static_cast<int64_t>(settings_.ble_observer_interval_s) * 1000;
    if (!mains_) battery_scan_done_ = true;
  }
  if (link_.load() != LinkState::None) onLinkLost();
  role_ = Role::Off;
  host_synced_ = false;
  host_stop_pending_ = false;
  host_failed_ = false;
  advertising_ = false;
  scan_complete_ = false;
  host_started_ms_ = 0;
  stop_retry_ms_ = 0;
  updateMemoryStatusLocked();
  ESP_LOGI(kTag, "host stopped and released");
  return true;
}

void BleManager::startObserverWindowLocked() {
  if (!host_synced_.load() || scanning_.load()) return;
  const int64_t now = nowMs();
  ble_gap_disc_params params = {};
  // 被动扫描 ~25% 占空比（interval 100ms / window 25ms，单位 0.625ms）：与 WiFi
  // 共用 2.4G 射频（软件共存），高占空比会挤占 STA 关联/握手时隙。
  params.itvl = 0xA0;
  params.window = 0x28;
  params.passive = 1;
  params.filter_duplicates = 0;
  scan_complete_ = false;
  const int rc = ble_gap_disc(g_own_addr_type,
                              static_cast<int32_t>(settings_.ble_observer_window_s * 1000), &params,
                              &BleManager::onScanEvent, this);
  if (rc != 0) {
    ESP_LOGW(kTag, "observer scan start rc=%d", rc);
    setErrorLocked("observer scan failed");
    next_observer_ms_ = now + static_cast<int64_t>(settings_.ble_observer_interval_s) * 1000;
    if (!mains_) battery_scan_done_ = true;
    return;
  }
  scanning_ = true;
  observer_end_ms_ = now + static_cast<int64_t>(settings_.ble_observer_window_s) * 1000 + 3000;
  ESP_LOGI(kTag, "observer window start (%lus)", static_cast<unsigned long>(settings_.ble_observer_window_s));
}

void BleManager::stopObserverWindowLocked() {
  if (!scanning_.load()) return;
  if (host_synced_.load() && !scan_complete_.load()) ble_gap_disc_cancel();
  scanning_ = false;
  scan_complete_ = false;
  next_observer_ms_ = nowMs() + static_cast<int64_t>(settings_.ble_observer_interval_s) * 1000;
  if (!mains_) battery_scan_done_ = true;
}

void BleManager::hostTask(void* arg) {
  (void)arg;
  nimble_port_run();
  nimble_port_freertos_deinit();
}

void BleManager::onSync() {
  BleManager& self = instance();
  if (self.host_stop_pending_) return;
  uint8_t own_addr_type = BLE_OWN_ADDR_PUBLIC;
  if (ble_hs_id_infer_auto(0, &own_addr_type) == 0) g_own_addr_type = own_addr_type;
  self.host_synced_ = true;
  if (self.admin_allowed_.load()) self.startAdvertising();
}

void BleManager::onReset(int reason) {
  instance().host_failed_ = true;
  instance().host_synced_ = false;
  ESP_LOGW(kTag, "NimBLE reset reason=%d", reason);
}

void BleManager::startAdvertising() {
  if (!host_synced_.load() || conn_handle_.load() != 0xffff || ble_gap_adv_active()) return;
  ble_hs_adv_fields fields = {};
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  fields.uuids128 = &kSvcUuid;
  fields.num_uuids128 = 1;
  fields.uuids128_is_complete = 1;
  int rc = ble_gap_adv_set_fields(&fields);
  if (rc == 0) {
    ble_hs_adv_fields rsp = {};
    rsp.name = reinterpret_cast<const uint8_t*>(device_name_.c_str());
    rsp.name_len = static_cast<uint8_t>(device_name_.size());
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
  }
  if (rc == 0) {
    ble_gap_adv_params params = {};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = BLE_GAP_ADV_ITVL_MS(300);
    params.itvl_max = BLE_GAP_ADV_ITVL_MS(500);
    rc = ble_gap_adv_start(g_own_addr_type, nullptr, BLE_HS_FOREVER, &params,
                           &BleManager::onConnEvent, this);
  }
  if (rc == 0) {
    advertising_ = true;
  } else if (rc != BLE_HS_EALREADY) {
    ESP_LOGW(kTag, "advertise start rc=%d", rc);
  }
}

// ---------------------------------------------------------------------------
// GAP / GATT 回调（NimBLE 主机任务）
// ---------------------------------------------------------------------------

int BleManager::onScanEvent(struct ble_gap_event* event, void* arg) {
  BleManager* self = static_cast<BleManager*>(arg);
  if (self == nullptr || event == nullptr) return 0;
  if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
    self->scan_complete_ = true;
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

void BleManager::onLinkLost() {
  const bool was_up = link_up_notified_.exchange(false);
  link_ = LinkState::None;
  conn_handle_ = 0xffff;
  tx_subscribed_ = false;
  passkey_valid_ = false;
  att_mtu_ = 23;
  TunnelHandler handler;
  {
    RxLock rx(rx_mutex_);
    if (rx_ != nullptr) rx_->reset();
    handler = handler_;
  }
  if (tx_sem_ != nullptr) xSemaphoreGive(static_cast<SemaphoreHandle_t>(tx_sem_));
  if (was_up && handler.onLinkDown != nullptr) handler.onLinkDown(handler.ctx);
}

int BleManager::onConnEvent(struct ble_gap_event* event, void* arg) {
  BleManager* self = static_cast<BleManager*>(arg);
  if (self == nullptr || event == nullptr) return 0;
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
      self->advertising_ = false;
      if (event->connect.status != 0) {
        if (self->admin_allowed_.load()) self->startAdvertising();
        return 0;
      }
      const uint16_t conn = event->connect.conn_handle;
      self->conn_handle_ = conn;
      self->link_ = LinkState::Connected;
      self->last_tunnel_ms_ = nowMs();
      ble_gap_set_data_len(conn, 251, 2120);
      ble_gap_upd_params params = {};
      params.itvl_min = BLE_GAP_CONN_ITVL_MS(15);
      params.itvl_max = BLE_GAP_CONN_ITVL_MS(30);
      params.latency = 0;
      params.supervision_timeout = BLE_GAP_SUPERVISION_TIMEOUT_MS(4000);
      ble_gap_update_params(conn, &params);
      // 主动请求安全：已绑定的对端直接重新加密，新对端触发配对码。
      const int sec_rc = ble_gap_security_initiate(conn);
      ESP_LOGI(kTag, "link connected (security initiate rc=%d)", sec_rc);
      return 0;
    }
    case BLE_GAP_EVENT_DISCONNECT:
      ESP_LOGI(kTag, "link disconnected reason=0x%x", event->disconnect.reason);
      self->onLinkLost();
      if (self->admin_allowed_.load() && !self->host_stop_pending_) self->startAdvertising();
      return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
      self->advertising_ = false;
      if (self->admin_allowed_.load() && self->conn_handle_.load() == 0xffff &&
          !self->host_stop_pending_) {
        self->startAdvertising();
      }
      return 0;
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
      ble_sm_io io = {};
      io.action = event->passkey.params.action;
      const int64_t now = nowMs();
      if (now < self->pair_lock_until_ms_.load()) {
        ESP_LOGW(kTag, "pairing refused: locked for %lld s",
                 static_cast<long long>((self->pair_lock_until_ms_.load() - now) / 1000));
        ble_gap_terminate(event->passkey.conn_handle, BLE_ERR_AUTH_FAIL);
        return 0;
      }
      if (io.action == BLE_SM_IOACT_DISP) {
        const uint32_t pk = esp_random() % 1000000u;
        io.passkey = pk;
        self->passkey_ = pk;
        self->passkey_deadline_ms_ = now + kPasskeyWindowMs;
        self->passkey_valid_ = true;
        self->link_ = LinkState::Pairing;
        ble_sm_inject_io(event->passkey.conn_handle, &io);
        ESP_LOGI(kTag, "pairing: passkey displayed");
      } else if (io.action == BLE_SM_IOACT_NUMCMP) {
        io.numcmp_accept = 0;  // 只接受配对码（DisplayOnly）
        ble_sm_inject_io(event->passkey.conn_handle, &io);
      } else {
        ble_gap_terminate(event->passkey.conn_handle, BLE_ERR_AUTH_FAIL);
      }
      return 0;
    }
    case BLE_GAP_EVENT_ENC_CHANGE: {
      const bool pairing_attempt = self->passkey_valid_.exchange(false);
      const uint16_t conn = event->enc_change.conn_handle;
      ble_gap_conn_desc desc = {};
      const bool found = ble_gap_conn_find(conn, &desc) == 0;
      ESP_LOGI(kTag, "enc change status=%d encrypted=%d authenticated=%d bonded=%d key_size=%d",
               event->enc_change.status, found && desc.sec_state.encrypted ? 1 : 0,
               found && desc.sec_state.authenticated ? 1 : 0, found && desc.sec_state.bonded ? 1 : 0,
               found ? desc.sec_state.key_size : 0);
      const bool ok = event->enc_change.status == 0 && found && desc.sec_state.encrypted &&
                      desc.sec_state.authenticated && desc.sec_state.key_size == 16;
      if (ok) {
        self->link_ = LinkState::Authenticated;
        self->pair_fail_count_ = 0;
        self->last_tunnel_ms_ = nowMs();
        TunnelHandler handler;
        {
          RxLock rx(self->rx_mutex_);
          if (self->rx_ != nullptr) self->rx_->reset();
          handler = self->handler_;
        }
        if (!self->link_up_notified_.exchange(true) && handler.onLinkUp != nullptr) {
          handler.onLinkUp(handler.ctx);
        }
        ESP_LOGI(kTag, "link authenticated (bonded=%d)", desc.sec_state.bonded ? 1 : 0);
      } else if (found && !pairing_attempt &&
                 event->enc_change.status == BLE_HS_HCI_ERR(BLE_ERR_PINKEY_MISSING)) {
        // 对端（电脑）已删除配对而本机仍有旧绑定：删掉旧绑定并重新发起配对，
        // 否则每次重连都会在这里失败。
        ESP_LOGW(kTag, "peer lost its bond; deleting ours and re-pairing");
        ble_store_util_delete_peer(&desc.peer_id_addr);
        self->link_ = LinkState::Connected;
        const int rc = ble_gap_security_initiate(conn);
        if (rc != 0) {
          ESP_LOGW(kTag, "security initiate rc=%d", rc);
          ble_gap_terminate(conn, BLE_ERR_AUTH_FAIL);
        }
      } else if (!pairing_attempt) {
        // 未显示配对码就失败（加密恢复中断等）：不计入锁定次数，断开让浏览器重试。
        self->link_ = LinkState::Connected;
        ESP_LOGW(kTag, "link encryption failed status=%d (no pairing attempt)", event->enc_change.status);
        ble_gap_terminate(conn, BLE_ERR_AUTH_FAIL);
      } else {
        const int64_t now = nowMs();
        if (now - self->pair_fail_window_ms_ > kPairFailWindowMs) {
          self->pair_fail_window_ms_ = now;
          self->pair_fail_count_ = 0;
        }
        if (++self->pair_fail_count_ >= kPairFailLimit) {
          self->pair_lock_until_ms_ = now + kPairLockMs;
          self->pair_fail_count_ = 0;
          ESP_LOGW(kTag, "pairing locked for 5 minutes");
        }
        self->link_ = LinkState::Connected;
        ESP_LOGW(kTag, "link security rejected status=%d", event->enc_change.status);
        ble_gap_terminate(conn, BLE_ERR_AUTH_FAIL);
      }
      return 0;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
      ble_gap_conn_desc desc = {};
      if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
        ble_store_util_delete_peer(&desc.peer_id_addr);
      }
      return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_MTU:
      self->att_mtu_ = event->mtu.value;
      return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
      if (event->subscribe.attr_handle == g_tx_handle) {
        self->tx_subscribed_ = event->subscribe.cur_notify != 0;
      }
      return 0;
    case BLE_GAP_EVENT_NOTIFY_TX:
      if (self->tx_sem_ != nullptr) xSemaphoreGive(static_cast<SemaphoreHandle_t>(self->tx_sem_));
      return 0;
    default:
      return 0;
  }
}

int BleManager::onGattAccess(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt* ctxt, void* arg) {
  (void)arg;
  BleManager& self = instance();
  if (ctxt == nullptr) return BLE_ATT_ERR_UNLIKELY;
  // 第二道防线：特征权限已要求加密+认证，这里再核对当前连接。
  ble_gap_conn_desc desc = {};
  if (ble_gap_conn_find(conn_handle, &desc) != 0 || !desc.sec_state.encrypted ||
      !desc.sec_state.authenticated || desc.sec_state.key_size != 16) {
    return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
  }
  if (attr_handle == g_info_handle && ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
    const std::string info = self.infoJson();
    return os_mbuf_append(ctxt->om, info.data(), info.size()) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
  }
  if (attr_handle == g_tx_handle && ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) return 0;
  if (attr_handle == g_rx_handle && ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
    uint8_t frame[kMaxFrameBytes];
    uint16_t len = 0;
    if (OS_MBUF_PKTLEN(ctxt->om) > sizeof(frame) ||
        ble_hs_mbuf_to_flat(ctxt->om, frame, sizeof(frame), &len) != 0) {
      return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    self.handleRx(conn_handle, frame, len);
    return 0;
  }
  return BLE_ATT_ERR_UNLIKELY;
}

void BleManager::sendControl(uint16_t conn, const uint8_t frame[4]) {
  // 主机任务内只尝试一次（NOTIFY_TX 事件也在本任务处理，不能在这里等待）。
  if (!tx_subscribed_.load()) return;
  os_mbuf* om = ble_hs_mbuf_from_flat(frame, tunnel::kHeaderBytes);
  if (om == nullptr) return;
  ble_gatts_notify_custom(conn, g_tx_handle, om);
}

void BleManager::handleRx(uint16_t conn, const uint8_t* data, size_t len) {
  last_tunnel_ms_ = nowMs();
  tunnel::FrameHeader header;
  if (!tunnel::parseHeader(data, len, header)) return;
  tunnel::Result result;
  uint16_t ack_seq = 0;
  bool ack = false;
  TunnelHandler handler;
  const uint8_t* msg = nullptr;
  size_t msg_len = 0;
  uint16_t head_len = 0;
  uint8_t msg_id = header.id;
  {
    RxLock rx(rx_mutex_);
    if (rx_ == nullptr) {
      rx_ = new tunnel::Reassembler(tunnel::kMaxRequestBytes, {&psramAlloc, &psramFree});
    }
    result = rx_->feed(data, len, nowMs());
    ack = rx_->ackDue(ack_seq);
    if (result == tunnel::Result::Complete) {
      msg = rx_->data();
      msg_len = rx_->size();
      head_len = rx_->headLen();
      msg_id = rx_->id();
    }
    handler = handler_;
  }
  if (ack) {
    uint8_t frame[4];
    tunnel::makeAck(frame, msg_id, ack_seq);
    sendControl(conn, frame);
  }
  switch (result) {
    case tunnel::Result::Ok:
    case tunnel::Result::Cancelled:
      return;
    case tunnel::Result::Complete:
      tunnel_messages_++;
      if (handler.onMessage != nullptr) {
        handler.onMessage(handler.ctx, msg_id, msg, msg_len, head_len);
      } else {
        releaseMessage();
      }
      return;
    default: {
      tunnel_errors_++;
      uint8_t frame[4];
      tunnel::makeAbort(frame, header.id, tunnel::abortReasonFor(result));
      sendControl(conn, frame);
      return;
    }
  }
}

void BleManager::releaseMessage() {
  RxLock rx(rx_mutex_);
  if (rx_ != nullptr && rx_->complete()) rx_->release();
}

bool BleManager::notifyFrame(uint16_t conn, const uint8_t* frame, size_t len, int64_t deadline_ms) {
  while (true) {
    if (link_.load() != LinkState::Authenticated || conn_handle_.load() != conn || !tx_subscribed_.load()) {
      return false;
    }
    if (nowMs() > deadline_ms) return false;
    os_mbuf* om = ble_hs_mbuf_from_flat(frame, static_cast<uint16_t>(len));
    if (om != nullptr) {
      const int rc = ble_gatts_notify_custom(conn, g_tx_handle, om);  // 总是消费 om
      if (rc == 0) return true;
      if (rc != BLE_HS_ENOMEM && rc != BLE_HS_EBUSY) {
        ESP_LOGW(kTag, "notify rc=%d", rc);
        return false;
      }
    }
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(tx_sem_), pdMS_TO_TICKS(10));
  }
}

esp_err_t BleManager::sendMessage(uint8_t id, const uint8_t* head, uint16_t head_len,
                                  const uint8_t* body, size_t body_len, uint32_t timeout_ms) {
  if (link_.load() != LinkState::Authenticated || !tx_subscribed_.load()) return ESP_ERR_INVALID_STATE;
  const size_t total = static_cast<size_t>(head_len) + body_len;
  if (total > tunnel::kMaxResponseBytes) return ESP_ERR_INVALID_SIZE;
  const uint16_t conn = conn_handle_.load();
  size_t chunk = static_cast<size_t>(att_mtu_.load()) - 3;
  if (chunk > 512) chunk = 512;
  if (chunk < tunnel::kMinChunk) chunk = tunnel::kMinChunk;  // MTU 23 → 20 字节帧仍可用
  const int64_t deadline = nowMs() + timeout_ms;
  xSemaphoreTake(g_send_mutex, portMAX_DELAY);
  const size_t frames = tunnel::encodeFrames(
      id, head, head_len, body, body_len, chunk,
      [&](const uint8_t* frame, size_t len) { return notifyFrame(conn, frame, len, deadline); });
  xSemaphoreGive(g_send_mutex);
  last_tunnel_ms_ = nowMs();
  if (frames == 0) {
    tunnel_errors_++;
    return ESP_FAIL;
  }
  return ESP_OK;
}

esp_err_t BleManager::sendEvent(const char* json) {
  if (json == nullptr) return ESP_ERR_INVALID_ARG;
  if (link_.load() != LinkState::Authenticated || !tx_subscribed_.load()) return ESP_ERR_INVALID_STATE;
  const size_t len = std::strlen(json);
  size_t chunk = static_cast<size_t>(att_mtu_.load()) - 3;
  if (chunk > 512) chunk = 512;
  if (len + tunnel::kHeaderBytes > chunk) return ESP_ERR_INVALID_SIZE;
  uint8_t frame[kMaxFrameBytes];
  tunnel::writeHeader(frame,
                      static_cast<uint8_t>((tunnel::kTypeEvent << 4) | tunnel::kFlagStart | tunnel::kFlagEnd),
                      0, 0);
  std::memcpy(frame + tunnel::kHeaderBytes, json, len);
  xSemaphoreTake(g_send_mutex, portMAX_DELAY);
  const bool ok = notifyFrame(conn_handle_.load(), frame, len + tunnel::kHeaderBytes, nowMs() + 500);
  xSemaphoreGive(g_send_mutex);
  return ok ? ESP_OK : ESP_FAIL;
}

// ---------------------------------------------------------------------------
// 广播监听结果处理（app_task）
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// 电源 / 恢复热点 / 绑定
// ---------------------------------------------------------------------------

// 配对进行中或监听窗口未结束时推迟深睡；空闲的管理连接不阻止休眠（请求本身经
// lastAuthenticatedRequestMs 延长交互期）。
bool BleManager::sleepBlocked() const { return passkey_valid_.load() || scanning_.load(); }

esp_err_t BleManager::beforeSleep() {
  if (linkAuthenticated()) sendEvent("{\"ev\":\"sleep\",\"in_s\":1}");
  lock();
  UnlockGuard guard{mutex_};
  admin_allowed_ = false;
  if (!stopHostLocked()) return ESP_ERR_INVALID_STATE;
  if (adv_queue_ != nullptr) xQueueReset(static_cast<QueueHandle_t>(adv_queue_));
  battery_window_open_ = false;
  battery_scan_done_ = false;
  return role_.load() == Role::Off ? ESP_OK : ESP_ERR_INVALID_STATE;
}

bool BleManager::beginRecoveryAp() {
  lock();
  UnlockGuard guard{mutex_};
  // 热点期间只暂停广播监听（扫描会把射频拉离 AP 信道）；管理广播保留，
  // 首次配置也可以走蓝牙。
  if (scanning_.load()) stopObserverWindowLocked();
  recovery_ap_active_ = true;
  return true;
}

void BleManager::endRecoveryAp() { recovery_ap_active_ = false; }

void BleManager::refreshBondCountLocked() {
  if (!host_synced_.load()) return;
  int count = 0;
  if (ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC, &count) == 0 && count >= 0) {
    bonds_ = static_cast<uint32_t>(count);
  }
}

esp_err_t BleManager::clearBonds() {
  lock();
  UnlockGuard guard{mutex_};
  esp_err_t err = ESP_OK;
  if (role_.load() != Role::Off && host_synced_.load()) {
    const uint16_t conn = conn_handle_.load();
    if (conn != 0xffff) ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    if (ble_store_clear() != 0) err = ESP_FAIL;
  } else {
    err = eego::net::CredentialStore::clearNamespace("nimble_bond");
  }
  if (err != ESP_OK) setErrorLocked("BLE bond erase failed");
  else bonds_ = 0;
  return err;
}

void BleManager::service(bool mains) {
  lock();
  UnlockGuard guard{mutex_};
  const int64_t now = nowMs();
  if (mains_ != mains) {
    if (scanning_.load()) stopObserverWindowLocked();
    mains_ = mains;
    battery_window_open_ = false;
    battery_scan_done_ = false;
    next_observer_ms_ = now;
  }
  if (now - memory_sample_ms_ >= 1000) {
    memory_sample_ms_ = now;
    updateMemoryStatusLocked();
    refreshBondCountLocked();
  }
  drainAdvertisementsLocked();

  if (passkey_valid_.load() && now > passkey_deadline_ms_.load()) passkey_valid_ = false;

  if (role_.load() == Role::Off) {
    if (wantHostLocked(now)) startHostLocked();
    return;
  }
  if (host_stop_pending_) {
    stopHostLocked();
    return;
  }
  if (host_failed_.exchange(false)) {
    setErrorLocked("NimBLE host reset");
    stopHostLocked();
    return;
  }
  if (scanning_.load() &&
      (scan_complete_.load() || now >= observer_end_ms_ || recovery_ap_active_.load())) {
    stopObserverWindowLocked();
  }
  if (!wantHostLocked(now)) {
    stopHostLocked();
    return;
  }
  role_ = admin_allowed_.load() ? Role::Admin : Role::Observer;
  if (!host_synced_.load()) return;
  if (!scanning_.load() && observerDueLocked(now)) startObserverWindowLocked();
  if (admin_allowed_.load()) {
    if (!ble_gap_adv_active() && conn_handle_.load() == 0xffff) startAdvertising();
  } else {
    if (ble_gap_adv_active()) ble_gap_adv_stop();
    advertising_ = false;
    const uint16_t conn = conn_handle_.load();
    if (conn != 0xffff) ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
  }
  // 半截上传超时：丢弃并通知浏览器重试。
  uint8_t timed_out_id = 0;
  bool timed_out = false;
  {
    RxLock rx(rx_mutex_);
    if (rx_ != nullptr && rx_->receiving()) {
      timed_out_id = rx_->id();
      timed_out = rx_->expire(now);
    }
  }
  if (timed_out) {
    uint8_t frame[4];
    tunnel::makeAbort(frame, timed_out_id, tunnel::AbortReason::Timeout);
    notifyFrame(conn_handle_.load(), frame, sizeof(frame), now + 50);
  }
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
  out.admin_allowed = admin_allowed_.load();
  out.advertising = role != Role::Off && advertising_.load();
  out.scanning = scanning_.load();
  out.link = link_.load();
  const int64_t now = nowMs();
  out.pairing_passkey_valid = passkey_valid_.load() && passkey_deadline_ms_.load() > now;
  if (out.pairing_passkey_valid) {
    out.pairing_passkey = passkey_.load();
    out.pairing_remaining_s = static_cast<uint32_t>((passkey_deadline_ms_.load() - now + 999) / 1000);
  }
  out.pairing_locked = now < pair_lock_until_ms_.load();
  out.bonds = bonds_;
  out.att_mtu = out.link == LinkState::None ? 0 : att_mtu_.load();
  out.device_name = device_name_;
  if (scanning_.load() && observer_end_ms_ > now) {
    out.observer_remaining_s = (observer_end_ms_ - now + 999) / 1000;
  }
  out.dropped_advertisements = dropped_advertisements_.load();
  out.emitted_events = emitted_events_;
  out.duplicate_advertisements = duplicate_advertisements_;
  out.malformed_advertisements = malformed_advertisements_;
  out.ingest_errors = ingest_errors_;
  out.tunnel_messages = tunnel_messages_.load();
  out.tunnel_errors = tunnel_errors_.load();
  out.diram_free_bytes = diram_free_bytes_;
  out.diram_min_free_bytes = diram_min_free_bytes_;
  out.psram_free_bytes = psram_free_bytes_;
  out.last_error = last_error_;
  return out;
}

}  // namespace eego::ble
