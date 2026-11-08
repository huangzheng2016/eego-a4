// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

//
//
//
//

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "alert_core/persistence.h"
#include "alert_core/snapshot.h"
#include "alert_core/store.h"
#include "alert_core/type_registry.h"
#include "esp_err.h"
#include "net_stack/alert_actions.h"
#include "net_stack/http_pool.h"
#include "net_stack/net_status.h"
#include "net_stack/time_sync.h"
#include "net_stack/wifi_list.h"
#include "net_stack/wifi_manager.h"

namespace eego::net {
enum class NetEventType : uint8_t {
  Started = 0,
  Stopped,
  WifiStateChanged,
};

struct NetEvent {
  NetEventType type = NetEventType::Started;
  int wifi_state = 0;
};

using NetEventCallback = void (*)(const NetEvent& ev, void* ctx);
using TimeFn = std::function<uint64_t()>;
using TimeValidFn = std::function<bool()>;

struct NetStackConfig {
  PoolConfig pool;
  bool wifi_auto_reconnect = true;
  uint32_t wifi_reconnect_backoff_max_ms = kWifiBackoffMaxMs;
  uint8_t task_priority = 5;
  uint16_t task_stack_bytes = 16384;
  int8_t task_core = 0;
  alert_core::SnapshotPublisher* snapshot_publisher = nullptr;
  uint32_t alert_expire_interval_ms = 15000;
  uint32_t alert_db_check_interval_ms = 600000;
  alert_core::AlertDb* alert_db = nullptr;
  alert_core::AlertStore::CompactPolicy alert_db_policy;
  std::vector<std::string> sntp_servers;
  TimeSync::RtcWriteFn rtc_write_fn;
  TimeValidFn time_valid_fn;
};

class NetStack {
 public:
  static NetStack& instance();

  esp_err_t start(const NetStackConfig& cfg, alert_core::AlertStore* store,
                  const alert_core::TypeRegistry* registry, TimeFn time_fn = nullptr);
  void stop();

  bool running() const { return running_; }

  bool reconnectNow(uint32_t wait_ms = 1000, bool force = false);
  void setWifiProvisioningHold(bool active) { wifi_provisioning_hold_.store(active); }
  bool wifiProvisioningHold() const { return wifi_provisioning_hold_.load(); }

  bool queryStatus(NetStatusSnapshot& out, uint32_t wait_ms = 1000);

  AlertActionResult alertAction(const AlertAction& action, uint32_t wait_ms = 5000);

  bool setMuteRules(const std::vector<alert_core::MuteRule>& rules, std::string& err,
                    uint32_t wait_ms = 5000);
  bool clearMuteRules(std::string& err, uint32_t wait_ms = 5000);
  bool queryMuteRules(std::vector<alert_core::MuteRule>& out, uint32_t wait_ms = 1000);

  AlertSnapshotSource alertSnapshotSource();
  AlertActionSink alertActionSink();
  bool republishSnapshot(uint32_t wait_ms = 1000);

  bool httpSubmit(const NetRequest& req, NetResponse* resp, uint32_t wait_ms = 10000);

  struct PushResult {
    enum class Code {
      Ok = 0,
      BadRequest,
      Oversize,
      Unavailable,
      StoreFull,
    } code = Code::Unavailable;
    int created = 0;
    int updated = 0;
    int rejected = 0;
    alert_core::ValidationError error;
  };
  PushResult pushIngest(const std::string& body, const std::string& sender,
                        uint32_t wait_ms = 5000);

  bool ingestExternal(const std::vector<alert_core::AlertEvent>& events,
                      const alert_core::AlertSource& source, uint32_t wait_ms = 5000);

  void setEventCallback(NetEventCallback cb, void* ctx);

  bool waitIdle(uint32_t wait_ms = 30000);

  WifiManager& wifi() { return wifi_; }
  const NetStackConfig& config() const { return cfg_; }

 private:
  NetStack();
  ~NetStack();
  NetStack(const NetStack&) = delete;
  NetStack& operator=(const NetStack&) = delete;

  enum class CmdType : int {
    ReconnectNow,
    HttpSubmit,
    Ingest,
    AlertActionCmd,
    SetMuteRules,
    QueryMuteRules,
    RepublishSnapshot,
    QueryStatus,
    WaitIdle,
    Stop,
  };

  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::unique_ptr<HttpPool> pool_;

  NetStackConfig cfg_;
  WifiManager& wifi_;
  alert_core::AlertStore* store_ = nullptr;
  const alert_core::TypeRegistry* registry_ = nullptr;
  TimeFn time_fn_;
  TimeSync time_sync_;
  NetEventCallback event_cb_ = nullptr;
  void* event_ctx_ = nullptr;
  bool running_ = false;
  std::atomic<bool> wifi_provisioning_hold_{false};

  bool sendCmdAndWait(CmdType type, const std::shared_ptr<void>& payload, uint32_t wait_ms);
  void emit(const NetEvent& ev);

  friend struct Impl;
};

}  // namespace eego::net
