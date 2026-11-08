// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// BLE 管理器：一个 NimBLE 主机，承载两类活动：
//   * 管理隧道（GATT 外设）：浏览器经 Web Bluetooth 连接，LE Secure Connections
//     配对码（设备屏幕显示 6 位）认证后，请求经 admin 组件的 ble_bridge 转发到本机
//     管理 API。帧格式见 ble/tunnel_frame.h。
//   * 广播监听（Observer）：按窗口被动扫描传感器广播，命中规则转为告警。
// 主机在「允许管理广播」或「监听窗口进行中」时常驻，两者都不需要时释放。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "ble/observer_core.h"
#include "ble/tunnel_frame.h"
#include "device_settings/settings_model.h"
#include "esp_err.h"
#include "host/ble_gap.h"

namespace eego::ble {

enum class Role : uint8_t {
  Off = 0,    // 主机未运行
  Observer,   // 仅监听窗口
  Admin,      // 管理广播/连接（可同时进行监听窗口）
};

enum class LinkState : uint8_t {
  None = 0,
  Connected,      // 已连接，未认证
  Pairing,        // 正在配对（屏幕显示配对码）
  Authenticated,  // 加密 + MITM 认证
};

struct Status {
  bool compiled = false;
  bool stack_active = false;
  bool recovery_ap_active = false;
  Role role = Role::Off;
  bool admin_allowed = false;
  bool advertising = false;
  bool scanning = false;
  LinkState link = LinkState::None;
  bool pairing_passkey_valid = false;
  uint32_t pairing_passkey = 0;
  uint32_t pairing_remaining_s = 0;
  bool pairing_locked = false;
  uint32_t bonds = 0;
  uint16_t att_mtu = 0;
  std::string device_name;
  uint32_t observer_remaining_s = 0;
  uint32_t dropped_advertisements = 0;
  uint32_t emitted_events = 0;
  uint32_t duplicate_advertisements = 0;
  uint32_t malformed_advertisements = 0;
  uint32_t ingest_errors = 0;
  uint32_t tunnel_messages = 0;
  uint32_t tunnel_errors = 0;
  uint32_t diram_free_bytes = 0;
  uint32_t diram_min_free_bytes = 0;
  uint32_t psram_free_bytes = 0;
  std::string last_error;
};

// 隧道消息处理器（admin/ble_bridge 实现）。回调在 NimBLE 主机任务中执行：
// 只能做入队，不得阻塞、不得调用 BleManager 的加锁接口。
struct TunnelHandler {
  void (*onLinkUp)(void* ctx) = nullptr;
  void (*onLinkDown)(void* ctx) = nullptr;
  // msg 在 releaseMessage() 之前一直有效（期间新请求会被 ABORT busy）。
  void (*onMessage)(void* ctx, uint8_t id, const uint8_t* msg, size_t len, uint16_t head_len) = nullptr;
  void* ctx = nullptr;
};

class BleManager {
 public:
  static BleManager& instance();
  static bool compiled();

  esp_err_t init();
  void configure(const eego::settings::Settings& settings);
  void onPowerMode(bool mains);
  void setBatteryScanWindow(bool open);
  void service(bool mains);
  // 管理网页正在使用时推迟广播监听窗口（扫描与 WiFi 共用射频，会让网页卡顿）。
  void deferObserverUntil(int64_t ms) { observer_defer_until_ms_.store(ms); }

  // 管理隧道
  void setAdminAllowed(bool allowed);
  void setTunnelHandler(const TunnelHandler& handler);
  // INFO 特征中的设备侧字段（由 bridge 维护）。
  void setTunnelInfo(const std::string& fw, bool provisioned, bool httpd);
  // 阻塞发送一条响应消息（bridge 任务调用）。
  esp_err_t sendMessage(uint8_t id, const uint8_t* head, uint16_t head_len, const uint8_t* body,
                        size_t body_len, uint32_t timeout_ms);
  esp_err_t sendEvent(const char* json);
  void releaseMessage();
  bool linkAuthenticated() const { return link_.load() == LinkState::Authenticated; }

  bool sleepBlocked() const;
  esp_err_t beforeSleep();

  bool beginRecoveryAp();
  void endRecoveryAp();

  esp_err_t clearBonds();
  Status status() const;

 private:
  BleManager() = default;
  std::atomic<int64_t> observer_defer_until_ms_{0};
  void lock() const;
  void unlock() const;
  bool wantHostLocked(int64_t now) const;
  bool observerDueLocked(int64_t now) const;
  esp_err_t startHostLocked();
  bool stopHostLocked();
  void startObserverWindowLocked();
  void stopObserverWindowLocked();
  void drainAdvertisementsLocked();
  void setErrorLocked(const std::string& error);
  void updateMemoryStatusLocked();
  void refreshBondCountLocked();

  // 主机任务上下文
  void startAdvertising();
  void setConnSpeed(uint16_t conn, bool fast);
  void handleRx(uint16_t conn, const uint8_t* data, size_t len);
  bool notifyFrame(uint16_t conn, const uint8_t* frame, size_t len, int64_t deadline_ms);
  void sendControl(uint16_t conn, const uint8_t frame[4]);
  void onLinkLost();
  std::string infoJson() const;

  struct RawAdvertisement;

  mutable void* mutex_ = nullptr;
  void* rx_mutex_ = nullptr;
  void* tx_sem_ = nullptr;
  void* adv_queue_ = nullptr;

  eego::settings::Settings settings_{};
  ObserverCore observer_;
  std::atomic<Role> role_{Role::Off};
  std::string last_error_;
  std::string device_name_;
  int64_t host_started_ms_ = 0;  // -1 = nimble_port_stop 已完成，待 deinit
  int64_t observer_end_ms_ = 0;
  int64_t next_observer_ms_ = 0;
  int64_t stop_retry_ms_ = 0;
  int64_t memory_sample_ms_ = 0;
  bool mains_ = true;
  bool battery_window_open_ = false;
  bool battery_scan_done_ = false;
  bool host_stop_pending_ = false;
  std::atomic<bool> host_synced_{false};
  std::atomic<bool> host_failed_{false};
  std::atomic<bool> scanning_{false};
  std::atomic<bool> scan_complete_{false};
  std::atomic<bool> recovery_ap_active_{false};
  std::atomic<bool> admin_allowed_{false};
  std::atomic<bool> advertising_{false};

  // 连接 / 配对（主机任务写，其他任务读）
  std::atomic<LinkState> link_{LinkState::None};
  std::atomic<uint16_t> conn_handle_{0xffff};
  std::atomic<uint16_t> att_mtu_{23};
  std::atomic<bool> tx_subscribed_{false};
  std::atomic<bool> passkey_valid_{false};
  std::atomic<uint32_t> passkey_{0};
  std::atomic<int64_t> passkey_deadline_ms_{0};
  std::atomic<int64_t> pair_lock_until_ms_{0};
  int64_t pair_fail_window_ms_ = 0;
  uint32_t pair_fail_count_ = 0;
  std::atomic<int64_t> last_tunnel_ms_{0};
  std::atomic<bool> conn_fast_{true};  // 连接参数：传输中快、空闲时慢（让射频给 WiFi）
  std::atomic<bool> link_up_notified_{false};
  uint32_t bonds_ = 0;

  TunnelHandler handler_{};
  tunnel::Reassembler* rx_ = nullptr;
  std::string info_fw_;
  std::atomic<bool> info_provisioned_{false};
  std::atomic<bool> info_httpd_{false};

  std::atomic<uint32_t> dropped_advertisements_{0};
  std::atomic<uint32_t> tunnel_messages_{0};
  std::atomic<uint32_t> tunnel_errors_{0};
  uint32_t emitted_events_ = 0;
  uint32_t duplicate_advertisements_ = 0;
  uint32_t malformed_advertisements_ = 0;
  uint32_t ingest_errors_ = 0;
  uint32_t diram_free_bytes_ = 0;
  uint32_t diram_min_free_bytes_ = 0;
  uint32_t psram_free_bytes_ = 0;

 public:
  // NimBLE C 回调（需要访问私有成员）
  static void hostTask(void* arg);
  static void onSync();
  static void onReset(int reason);
  static int onScanEvent(struct ble_gap_event* event, void* arg);
  static int onConnEvent(struct ble_gap_event* event, void* arg);
  static int onGattAccess(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt* ctxt, void* arg);
};

const char* roleName(Role role);
const char* linkStateName(LinkState state);

}  // namespace eego::ble
