// SPDX-License-Identifier: GPL-3.0-or-later
#include "net_stack/net_task.h"

#include <cstdint>
#include <algorithm>
#include <cstring>
#include <ctime>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/platform.h"

#include "store_digest.h"

namespace eego::net {
static const char* TAG = "net.task";

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
struct PendingOp {
  SemaphoreHandle_t sem;
  bool ok = false;
  std::string err;
  PendingOp() : sem(xSemaphoreCreateBinary()) {}
  ~PendingOp() {
    if (sem) vSemaphoreDelete(sem);
  }
  void give() {
    if (sem) xSemaphoreGive(sem);
  }
};

struct NetStack::Impl {
  struct Command {
    CmdType type;
    std::shared_ptr<void> payload;
  };

  struct IdPayload {  // ReconnectNow
    std::string id;
    bool force = false;
    PendingOp op;
  };
  struct HttpPayload {
    NetRequest req;
    NetResponse resp;
    PendingOp op;
  };
  struct IngestPayload {
    std::vector<alert_core::AlertEvent> events;
    alert_core::AlertSource source;
    std::vector<alert_core::IngestOutcome> outcomes;
    PendingOp op;
  };
  struct AlertActionPayload {
    AlertAction action;
    AlertActionResult result;
    PendingOp op;
  };
  struct MuteRulesPayload {
    std::vector<alert_core::MuteRule> rules;
    PendingOp op;
  };
  struct StatusPayload {
    NetStatusSnapshot snapshot;
    PendingOp op;
  };
  struct SemPayload {  // WaitIdle / Stop / RepublishSnapshot
    PendingOp op;
  };

  NetStack* self = nullptr;
  QueueHandle_t queue = nullptr;
  TaskHandle_t task = nullptr;

  WifiReconnectScheduler wifi_sched_;
  uint32_t wifi_disconnects_ = 0;
  WifiState last_wifi_state_ = WifiState::Disconnected;
  bool wifi_provisioning_was_held_ = false;

  uint64_t last_digest_ = 0;
  bool have_digest_ = false;
  int64_t next_expire_ms_ = 0;
  int64_t next_db_check_ms_ = 0;
  uint32_t db_compactions_ = 0;
  uint64_t db_last_compact_at_ = 0;

  static int64_t nowMs() { return esp_timer_get_time() / 1000; }
  uint64_t nowUnix() const {
    return self->time_fn_ ? self->time_fn_() : (uint64_t)time(nullptr);
  }
  bool timeValid() const {
    if (self->cfg_.time_valid_fn) return self->cfg_.time_valid_fn();
    return nowUnix() >= kPlausibleUnixFloor;
  }

  void publishSnapshotIfChanged(bool force) {
    if (!self->store_ || !self->cfg_.snapshot_publisher) return;
    const uint64_t d = internal::alertStoreDigest(*self->store_);
    if (!force && have_digest_ && d == last_digest_) return;
    auto* pub = self->cfg_.snapshot_publisher;
    pub->publish(alert_core::buildSnapshot(self->store_->allRecords(), nowUnix(),
                                           pub->publishedSeq() + 1));
    last_digest_ = d;
    have_digest_ = true;
  }

  void expireDue(int64_t now) {
    if (!self->store_ || self->cfg_.alert_expire_interval_ms == 0) return;
    if (now < next_expire_ms_) return;
    next_expire_ms_ = now + self->cfg_.alert_expire_interval_ms;
    const uint64_t unix_now = nowUnix();
    const size_t n = self->store_->expire(unix_now);
    if (n > 0) {
      ESP_LOGI(TAG, "alert expire: %u record(s) expired", (unsigned)n);
    }
    self->store_->setMuteRules(self->store_->muteRules(), unix_now);
    publishSnapshotIfChanged(false);
  }

  void maintainAlertDb(int64_t now) {
    if (!self->store_ || self->cfg_.alert_db_check_interval_ms == 0) return;
    if (now < next_db_check_ms_) return;
    next_db_check_ms_ = now + self->cfg_.alert_db_check_interval_ms;
    constexpr size_t kCompactMinFree = 32 * 1024;
    constexpr size_t kCompactMinBlock = 12 * 1024;
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < kCompactMinFree ||
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) <
            kCompactMinBlock) {
      return;
    }
    std::string err;
    bool attempted = false;
    if (self->store_->maybeCompactDb(self->cfg_.alert_db_policy, err, &attempted)) {
      ++db_compactions_;
      db_last_compact_at_ = nowUnix();
      ESP_LOGI(TAG, "alerts.db compacted: %u record(s), %u bytes",
               (unsigned)self->store_->size(), (unsigned)self->store_->lastCompactBytes());
    } else if (attempted) {
      ESP_LOGW(TAG, "alerts.db compaction failed: %s", err.c_str());
    }
  }

  void process(Command* cmd) {
    std::unique_ptr<Command> holder(cmd);
    switch (cmd->type) {
      case CmdType::ReconnectNow: {
        auto p = std::static_pointer_cast<IdPayload>(cmd->payload);
        if (p->force && self->wifi_.isConnected()) {
          ESP_LOGI(TAG, "wifi list changed; reconnecting from top priority");
          self->wifi_.disconnect();
        }
        wifi_sched_.kick(nowMs());
        p->op.ok = true;
        p->op.give();
        break;
      }
      case CmdType::HttpSubmit: {
        auto p = std::static_pointer_cast<HttpPayload>(cmd->payload);
        if (self->pool_) p->resp = self->pool_->execute(p->req);
        p->op.ok = true;
        p->op.give();
        break;
      }
      case CmdType::Ingest: {
        auto p = std::static_pointer_cast<IngestPayload>(cmd->payload);
        p->outcomes.clear();
        p->outcomes.reserve(p->events.size());
        uint64_t now = nowUnix();
        for (const auto& ev : p->events) {
          if (self->store_) {
            p->outcomes.push_back(self->store_->ingest(ev, p->source, now));
          } else {
            p->outcomes.push_back(alert_core::IngestOutcome{});
          }
        }
        p->op.ok = true;
        publishSnapshotIfChanged(false);
        p->op.give();
        break;
      }
      case CmdType::AlertActionCmd: {
        auto p = std::static_pointer_cast<AlertActionPayload>(cmd->payload);
        AlertActionResult& r = p->result;
        if (!self->store_) {
          r.code = AlertActionResult::Code::NoStore;
          r.detail = "alert store not attached";
        } else {
          const uint64_t now = nowUnix();
          const std::string& id = p->action.id;
          switch (p->action.type) {
            case AlertActionType::Ack:
              if (id.empty()) {
                r.code = AlertActionResult::Code::InvalidAction;
                r.detail = "ack requires alert id";
              } else if (self->store_->ack(id, p->action.ack_by, now)) {
                r.code = AlertActionResult::Code::Ok;
                r.affected = 1;
                publishSnapshotIfChanged(false);
              } else if (self->store_->find(id)) {
                r.code = AlertActionResult::Code::InvalidState;
                r.detail = "alert not active (already acked/cleared/expired)";
              } else {
                r.code = AlertActionResult::Code::NotFound;
                r.detail = "alert id not found";
              }
              break;
            case AlertActionType::AckAll: {
              const size_t n = self->store_->ackAll(p->action.ack_by, now);
              r.code = AlertActionResult::Code::Ok;
              r.affected = n;
              if (n > 0) publishSnapshotIfChanged(false);
              break;
            }
            case AlertActionType::Clear:
              if (id.empty()) {
                r.code = AlertActionResult::Code::InvalidAction;
                r.detail = "clear requires alert id";
              } else if (self->store_->clear(id, now)) {
                r.code = AlertActionResult::Code::Ok;
                r.affected = 1;
                publishSnapshotIfChanged(false);
              } else if (self->store_->find(id)) {
                r.code = AlertActionResult::Code::InvalidState;
                r.detail = "alert already cleared/expired";
              } else {
                r.code = AlertActionResult::Code::NotFound;
                r.detail = "alert id not found";
              }
              break;
          }
        }
        p->op.ok = (r.code == AlertActionResult::Code::Ok);
        p->op.give();
        break;
      }
      case CmdType::SetMuteRules: {
        auto p = std::static_pointer_cast<MuteRulesPayload>(cmd->payload);
        if (!self->store_) {
          p->op.ok = false;
          p->op.err = "alert store not attached";
        } else {
          self->store_->setMuteRules(p->rules, nowUnix());
          publishSnapshotIfChanged(false);
          p->op.ok = true;
        }
        p->op.give();
        break;
      }
      case CmdType::QueryMuteRules: {
        auto p = std::static_pointer_cast<MuteRulesPayload>(cmd->payload);
        p->rules.clear();
        if (self->store_) p->rules = self->store_->muteRules();
        p->op.ok = (self->store_ != nullptr);
        if (!p->op.ok) p->op.err = "alert store not attached";
        p->op.give();
        break;
      }
      case CmdType::RepublishSnapshot: {
        auto p = std::static_pointer_cast<SemPayload>(cmd->payload);
        publishSnapshotIfChanged(true);
        p->op.ok = true;
        p->op.give();
        break;
      }
      case CmdType::QueryStatus: {
        auto p = std::static_pointer_cast<StatusPayload>(cmd->payload);
        p->snapshot = NetStatusSnapshot{};
        p->snapshot.wifi.state = (int)self->wifi_.state();
        p->snapshot.wifi.ssid = self->wifi_.currentSsid();
        p->snapshot.wifi.rssi = self->wifi_.rssi();
        p->snapshot.wifi.ip = self->wifi_.currentIp();
        p->snapshot.wifi.last_reason = self->wifi_.lastReason();
        p->snapshot.wifi.reconnect_backoff_ms = wifi_sched_.backoffMs();
        p->snapshot.wifi.last_disconnect_reason = self->wifi_.lastDisconnectReason();
        p->snapshot.wifi.reconnect_attempts = wifi_sched_.attempts();
        p->snapshot.wifi.disconnects = wifi_disconnects_;
        p->snapshot.wifi.driver_restarts = self->wifi_.driverRestarts();
        {
          const WifiManager::LinkInfo link = self->wifi_.linkInfo();
          p->snapshot.wifi.rssi_samples = link.rssi.samples;
          p->snapshot.wifi.rssi_min = link.rssi.min;
          p->snapshot.wifi.rssi_avg = link.rssi.avg;
          p->snapshot.wifi.beacon_timeouts = link.beacon_timeouts;
          p->snapshot.wifi.roams = link.roams;
          p->snapshot.wifi.last_dhcp_ms = link.last_dhcp_ms;
          p->snapshot.wifi.tx_power_qdbm = link.tx_power_qdbm;
        }
        {
          const int64_t since = wifi_sched_.offlineSinceMs();
          p->snapshot.wifi.offline_ms =
              since < 0 ? 0 : static_cast<uint32_t>(std::min<int64_t>(nowMs() - since, UINT32_MAX));
        }
        p->snapshot.pool_sessions = self->pool_ ? self->pool_->sessionCount() : 0;
        p->snapshot.running = true;
        if (self->store_) {
          AlertDbStatus& db = p->snapshot.alert_db;
          db.records = self->store_->size();
          db.io_failures = self->store_->dbIoFailures();
          db.compactions = db_compactions_;
          db.last_compact_at = db_last_compact_at_;
          if (self->cfg_.alert_db != nullptr) {
            db.attached = true;
            self->cfg_.alert_db->fileSize(db.file_bytes);
          }
        }
        p->snapshot.time.valid = timeValid();
        p->snapshot.time.now = nowUnix();
        const TimeSyncStatus ts = self->time_sync_.status();
        p->snapshot.time.sntp_synced = ts.sntp_synced;
        p->snapshot.time.last_sync_at = ts.last_sync_at;
        p->op.ok = true;
        p->op.give();
        break;
      }
      case CmdType::WaitIdle: {
        auto p = std::static_pointer_cast<SemPayload>(cmd->payload);
        p->op.ok = true;
        p->op.give();
        break;
      }
      case CmdType::Stop: {
        auto p = std::static_pointer_cast<SemPayload>(cmd->payload);
        p->op.ok = true;
        p->op.give();
        self->running_ = false;
        break;
      }
    }
  }

  void pumpWifi(int64_t now) {
    if (!self->cfg_.wifi_auto_reconnect) return;
    const bool provisioning_hold = self->wifiProvisioningHold();
    if (provisioning_hold) {
      wifi_provisioning_was_held_ = true;
      wifi_sched_.deferUntil(now + 1000);
      return;
    }
    if (wifi_provisioning_was_held_) {
      wifi_provisioning_was_held_ = false;
      wifi_sched_.kick(now);
    }
    WifiState st = self->wifi_.state();
    if (st == WifiState::Connected) {
      if (last_wifi_state_ != WifiState::Connected) {
        wifi_sched_.onConnected(now);
        self->time_sync_.onWifiConnected();
      }
      if (self->wifi_.serviceLink(now)) {
        std::vector<WifiAccount> accounts;
        CredentialStore::loadWifiAccounts(accounts);
        self->wifi_.roam(accounts);
        for (auto& account : accounts) CredentialStore::secureErase(account.password);
      }
    } else if (st != WifiState::Connecting) {
      if (last_wifi_state_ == WifiState::Connected) {
        wifi_disconnects_++;
        ESP_LOGW(TAG, "wifi link lost (reason=%u); reconnecting through saved networks",
                 (unsigned)self->wifi_.lastDisconnectReason());
        wifi_sched_.kick(now);
      }
      wifi_sched_.onOffline(now);
      if (wifi_sched_.due(now)) {
        std::vector<WifiAccount> accounts;
        CredentialStore::loadWifiAccounts(accounts);
        if (accounts.empty()) {
          wifi_sched_.onAttemptFailed(now);
        } else {
          if (wifi_sched_.driverRestartDue(now)) {
            ESP_LOGW(TAG, "wifi offline too long; restarting wifi driver");
            self->wifi_.restartDriver();
            wifi_sched_.onDriverRestarted(now);
          }
          ESP_LOGI(TAG, "wifi reconnect attempt #%u (%u networks, backoff %ums)",
                   (unsigned)wifi_sched_.attempts() + 1, (unsigned)accounts.size(),
                   (unsigned)wifi_sched_.backoffMs());
          self->wifi_.connectMulti(accounts);
          for (auto& account : accounts) CredentialStore::secureErase(account.password);
          if (self->wifi_.state() != WifiState::Connected) {
            const int64_t after = nowMs();
            wifi_sched_.onAttemptFailed(after);
            ESP_LOGW(TAG, "wifi reconnect failed (last reason=%u); next try in %lldms",
                     (unsigned)self->wifi_.lastDisconnectReason(),
                     (long long)(wifi_sched_.nextRetryMs() - after));
          }
        }
      }
      st = self->wifi_.state();
      if (st == WifiState::Connected) {
        wifi_sched_.onConnected(now);
        self->time_sync_.onWifiConnected();
      }
    }
    if (st != last_wifi_state_) {
      last_wifi_state_ = st;
      self->emit({NetEventType::WifiStateChanged, (int)st});
    }
  }

  int64_t nextWakeMs(int64_t now) {
    int64_t next = now + 1000;
    const int64_t retry = wifi_sched_.nextRetryMs();
    if (retry > 0 && retry < next) next = retry;
    return std::max<int64_t>(next - now, 10);
  }

  static void taskTrampoline(void* arg) { static_cast<Impl*>(arg)->loop(); }

  void loop() {
    publishSnapshotIfChanged(true);
    self->emit({NetEventType::Started, 0});
    while (self->running_) {
      int64_t now = nowMs();
      int processed = 0;
      Command* cmd = nullptr;
      while (processed < 16 && xQueueReceive(queue, &cmd, 0) == pdTRUE) {
        process(cmd);
        processed++;
      }
      if (!self->running_) break;
      now = nowMs();
      pumpWifi(now);
      expireDue(now);
      maintainAlertDb(now);
      self->time_sync_.service();
      if (self->pool_) self->pool_->housekeeping(now);
      TickType_t wait = pdMS_TO_TICKS((uint32_t)nextWakeMs(now));
      if (xQueueReceive(queue, &cmd, wait) == pdTRUE) process(cmd);
    }
    self->emit({NetEventType::Stopped, 0});
    task = nullptr;
    vTaskDelete(nullptr);
  }
};

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
NetStack& NetStack::instance() {
  static NetStack inst;
  return inst;
}

NetStack::NetStack() : wifi_(WifiManager::instance()) {}

NetStack::~NetStack() {
  stop();
  if (impl_ && impl_->queue) vQueueDelete(impl_->queue);
}

esp_err_t NetStack::start(const NetStackConfig& cfg, alert_core::AlertStore* store,
                          const alert_core::TypeRegistry* registry, TimeFn time_fn) {
  if (running_) return ESP_ERR_INVALID_STATE;
  if (!store || !registry) return ESP_ERR_INVALID_ARG;
  cfg_ = cfg;
  store_ = store;
  registry_ = registry;
  time_fn_ = std::move(time_fn);
  pool_ = std::make_unique<HttpPool>(cfg.pool);

  mbedtls_platform_set_calloc_free(
      [](size_t n, size_t size) -> void* {
        if (size != 0 && n > SIZE_MAX / size) return nullptr;
        // Small TLS blocks prefer internal RAM (speed) through the default heap, so they
        // never consume the internal reserve kept for DMA; everything else uses PSRAM.
        if (n * size <= 4096) {
          return heap_caps_calloc_prefer(n, size, 2, MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL,
                                         MALLOC_CAP_DEFAULT | MALLOC_CAP_SPIRAM);
        }
        return heap_caps_calloc_prefer(n, size, 2, MALLOC_CAP_DEFAULT | MALLOC_CAP_SPIRAM,
                                       MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
      },
      heap_caps_free);

  if (!impl_) impl_ = std::make_unique<Impl>();
  impl_->self = this;
  impl_->wifi_sched_ = WifiReconnectScheduler(kWifiBackoffStartMs, cfg.wifi_reconnect_backoff_max_ms);
  impl_->wifi_disconnects_ = 0;
  impl_->last_wifi_state_ = WifiState::Disconnected;
  impl_->last_digest_ = 0;
  impl_->have_digest_ = false;
  impl_->next_expire_ms_ = 0;
  impl_->next_db_check_ms_ = Impl::nowMs() + cfg.alert_db_check_interval_ms;
  impl_->db_compactions_ = 0;
  impl_->db_last_compact_at_ = 0;
  time_sync_.configure(cfg.sntp_servers, cfg.rtc_write_fn);
  if (!impl_->queue) impl_->queue = xQueueCreate(16, sizeof(Impl::Command*));
  if (!impl_->queue) return ESP_ERR_NO_MEM;

  esp_err_t err = wifi_.init();
  if (err != ESP_OK) return err;

  running_ = true;
  BaseType_t ok = xTaskCreatePinnedToCore(
      &Impl::taskTrampoline, "net_task", cfg.task_stack_bytes / sizeof(StackType_t),
      impl_.get(), cfg.task_priority, &impl_->task, cfg.task_core);
  if (ok != pdPASS) {
    running_ = false;
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void NetStack::stop() {
  if (!running_ || !impl_) return;
  auto p = std::make_shared<Impl::SemPayload>();
  Impl::Command* cmd = new Impl::Command{CmdType::Stop, p};
  if (xQueueSend(impl_->queue, &cmd, pdMS_TO_TICKS(1000)) == pdTRUE) {
    xSemaphoreTake(p->op.sem, pdMS_TO_TICKS(5000));
    for (int i = 0; i < 100 && running_; i++) vTaskDelay(pdMS_TO_TICKS(50));
  } else {
    delete cmd;
  }
  running_ = false;
}

bool NetStack::sendCmdAndWait(CmdType type, const std::shared_ptr<void>& payload,
                              uint32_t wait_ms) {
  if (!running_ || !impl_ || !impl_->queue) return false;
  Impl::Command* cmd = new Impl::Command{type, payload};
  if (xQueueSend(impl_->queue, &cmd, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
    delete cmd;
    return false;
  }
  return true;
}

bool NetStack::reconnectNow(uint32_t wait_ms, bool force) {
  auto p = std::make_shared<Impl::IdPayload>();
  p->force = force;
  if (!sendCmdAndWait(CmdType::ReconnectNow, p, wait_ms) ||
      xSemaphoreTake(p->op.sem, pdMS_TO_TICKS(wait_ms)) != pdTRUE)
    return false;
  return p->op.ok;
}

bool NetStack::queryStatus(NetStatusSnapshot& out, uint32_t wait_ms) {
  auto p = std::make_shared<Impl::StatusPayload>();
  if (!sendCmdAndWait(CmdType::QueryStatus, p, wait_ms) ||
      xSemaphoreTake(p->op.sem, pdMS_TO_TICKS(wait_ms)) != pdTRUE)
    return false;
  out = std::move(p->snapshot);
  return p->op.ok;
}

AlertActionResult NetStack::alertAction(const AlertAction& action, uint32_t wait_ms) {
  AlertActionResult r;
  auto p = std::make_shared<Impl::AlertActionPayload>();
  p->action = action;
  if (!sendCmdAndWait(CmdType::AlertActionCmd, p, wait_ms) ||
      xSemaphoreTake(p->op.sem, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
    r.code = AlertActionResult::Code::Unavailable;
    r.detail = "net_task busy/unavailable";
    return r;
  }
  return p->result;
}

AlertSnapshotSource NetStack::alertSnapshotSource() {
  alert_core::SnapshotPublisher* pub = cfg_.snapshot_publisher;
  return [pub]() -> std::shared_ptr<const alert_core::AlertSnapshot> {
    if (!pub) return nullptr;
    return pub->acquire();
  };
}

AlertActionSink NetStack::alertActionSink() {
  return [this](const AlertAction& action) { return alertAction(action); };
}

bool NetStack::republishSnapshot(uint32_t wait_ms) {
  auto p = std::make_shared<Impl::SemPayload>();
  if (!sendCmdAndWait(CmdType::RepublishSnapshot, p, wait_ms) ||
      xSemaphoreTake(p->op.sem, pdMS_TO_TICKS(wait_ms)) != pdTRUE)
    return false;
  return p->op.ok;
}

bool NetStack::setMuteRules(const std::vector<alert_core::MuteRule>& rules, std::string& err,
                            uint32_t wait_ms) {
  auto p = std::make_shared<Impl::MuteRulesPayload>();
  p->rules = rules;
  if (!sendCmdAndWait(CmdType::SetMuteRules, p, wait_ms) ||
      xSemaphoreTake(p->op.sem, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
    err = "net_task busy/unavailable";
    return false;
  }
  err = p->op.err;
  return p->op.ok;
}

bool NetStack::clearMuteRules(std::string& err, uint32_t wait_ms) {
  return setMuteRules({}, err, wait_ms);
}

bool NetStack::queryMuteRules(std::vector<alert_core::MuteRule>& out, uint32_t wait_ms) {
  auto p = std::make_shared<Impl::MuteRulesPayload>();
  if (!sendCmdAndWait(CmdType::QueryMuteRules, p, wait_ms) ||
      xSemaphoreTake(p->op.sem, pdMS_TO_TICKS(wait_ms)) != pdTRUE)
    return false;
  out = std::move(p->rules);
  return p->op.ok;
}

const char* toString(AlertActionResult::Code code) {
  switch (code) {
    case AlertActionResult::Code::Ok: return "ok";
    case AlertActionResult::Code::NotFound: return "not_found";
    case AlertActionResult::Code::InvalidState: return "invalid_state";
    case AlertActionResult::Code::InvalidAction: return "invalid_action";
    case AlertActionResult::Code::Unavailable: return "unavailable";
    case AlertActionResult::Code::NoStore: return "no_store";
  }
  return "unknown";
}

bool NetStack::httpSubmit(const NetRequest& req, NetResponse* resp, uint32_t wait_ms) {
  if (!resp) return false;
  auto p = std::make_shared<Impl::HttpPayload>();
  p->req = req;
  if (!sendCmdAndWait(CmdType::HttpSubmit, p, wait_ms) ||
      xSemaphoreTake(p->op.sem, pdMS_TO_TICKS(wait_ms)) != pdTRUE)
    return false;
  *resp = std::move(p->resp);
  return p->op.ok;
}

NetStack::PushResult NetStack::pushIngest(const std::string& body, const std::string& sender,
                                          uint32_t wait_ms) {
  PushResult r;
  if (body.size() > alert_core::kMaxBatchEvents * alert_core::kMaxEventJsonBytes) {
    r.code = PushResult::Code::Oversize;
    return r;
  }
  std::vector<alert_core::AlertEvent> events;
  alert_core::ValidationError verr;
  if (!alert_core::eventsFromJsonText(body, events, verr) || events.empty()) {
    if (events.empty() && verr.field.empty() && verr.message.empty()) {
      verr.message = "no alert events in body";
    }
    r.code = PushResult::Code::BadRequest;
    r.error = verr;
    return r;
  }
  auto p = std::make_shared<Impl::IngestPayload>();
  p->events = std::move(events);
  p->source = {alert_core::kSourceKindHttpPush, alert_core::kSourceKindHttpPush, sender};
  if (!sendCmdAndWait(CmdType::Ingest, p, wait_ms) ||
      xSemaphoreTake(p->op.sem, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
    r.code = PushResult::Code::Unavailable;
    return r;
  }
  for (const auto& o : p->outcomes) {
    switch (o.status) {
      case alert_core::IngestOutcome::Status::Created: r.created++; break;
      case alert_core::IngestOutcome::Status::Updated: r.updated++; break;
      case alert_core::IngestOutcome::Status::Rejected: r.rejected++; break;
    }
  }
  r.code = (r.rejected > 0 && r.created == 0 && r.updated == 0) ? PushResult::Code::StoreFull
                                                                : PushResult::Code::Ok;
  return r;
}

bool NetStack::ingestExternal(const std::vector<alert_core::AlertEvent>& events,
                              const alert_core::AlertSource& source, uint32_t wait_ms) {
  if (events.empty()) return false;
  auto p = std::make_shared<Impl::IngestPayload>();
  p->events = events;
  p->source = source;
  if (!sendCmdAndWait(CmdType::Ingest, p, wait_ms) ||
      xSemaphoreTake(p->op.sem, pdMS_TO_TICKS(wait_ms)) != pdTRUE)
    return false;
  return p->op.ok;
}

void NetStack::setEventCallback(NetEventCallback cb, void* ctx) {
  event_cb_ = cb;
  event_ctx_ = ctx;
}

bool NetStack::waitIdle(uint32_t wait_ms) {
  auto p = std::make_shared<Impl::SemPayload>();
  if (!sendCmdAndWait(CmdType::WaitIdle, p, wait_ms) ||
      xSemaphoreTake(p->op.sem, pdMS_TO_TICKS(wait_ms)) != pdTRUE)
    return false;
  return p->op.ok;
}

void NetStack::emit(const NetEvent& ev) {
  if (event_cb_) event_cb_(ev, event_ctx_);
}

}  // namespace eego::net
