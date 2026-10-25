// SPDX-License-Identifier: GPL-3.0-or-later
// net_task 实现 —— 唯一 esp_http_client 属主 + 唯一 AlertStore 写方（M12）。
#include "net_stack/net_task.h"

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

// WiFi 指数退避（M13 F3）：30s → 60s → 120s → 240s → 300s 封顶
static const uint32_t kWifiBackoffStartMs = 30000;

// ---------------------------------------------------------------------------
// 命令队列模型。
// * 队列元素是 Command*（平凡可拷贝，FreeRTOS 队列只 memcpy 指针）；
// * Command 持有 shared_ptr 负载 → 调用方超时先返回也绝无悬垂（net_task
//   处理完才释放最后的引用）；负载内 PendingOp 自建/自删二进制信号量，
//   net_task 对无等待者的信号量 give 无害。
// ---------------------------------------------------------------------------
struct PendingOp {
  SemaphoreHandle_t sem;
  bool ok = false;
  std::string err;
  PendingOp() : sem(xSemaphoreCreateBinary()) {}
  ~PendingOp() {
    if (sem) vSemaphoreDelete(sem);
  }
  // 对无等待者/已超时返回的调用方 give 无害（信号量随负载存活）
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
    PendingOp op;
  };
  struct HttpPayload {
    NetRequest req;
    NetResponse resp;  // net_task 写入；take 成功后调用方 move 出（防超时 UAF）
    PendingOp op;
  };
  struct IngestPayload {
    std::vector<alert_core::AlertEvent> events;
    alert_core::AlertSource source;
    std::vector<alert_core::IngestOutcome> outcomes;  // net_task 回填
    PendingOp op;
  };
  struct AlertActionPayload {
    AlertAction action;
    AlertActionResult result;  // net_task 回填
    PendingOp op;
  };
  struct MuteRulesPayload {
    std::vector<alert_core::MuteRule> rules;  // Set 输入 / Query 输出
    PendingOp op;
  };
  struct StatusPayload {
    NetStatusSnapshot snapshot;  // net_task 写入；take 成功后调用方 move 出
    PendingOp op;
  };
  struct SemPayload {  // WaitIdle / Stop / RepublishSnapshot
    PendingOp op;
  };

  NetStack* self = nullptr;
  QueueHandle_t queue = nullptr;
  TaskHandle_t task = nullptr;

  // WiFi 重连状态（net_task 上下文私有）
  int64_t next_wifi_retry_ms_ = 0;
  uint32_t wifi_backoff_ms_ = kWifiBackoffStartMs;
  WifiState last_wifi_state_ = WifiState::Disconnected;
  bool wifi_provisioning_was_held_ = false;

  // 快照发布状态（net_task 上下文私有）：摘要变了才重建发布
  uint64_t last_digest_ = 0;
  bool have_digest_ = false;
  int64_t next_expire_ms_ = 0;
  // alerts.db 维护（net_task 私有）
  int64_t next_db_check_ms_ = 0;
  uint32_t db_compactions_ = 0;
  uint64_t db_last_compact_at_ = 0;

  static int64_t nowMs() { return esp_timer_get_time() / 1000; }
  uint64_t nowUnix() const {
    return self->time_fn_ ? self->time_fn_() : (uint64_t)time(nullptr);
  }
  // 统一时间源可信判定（M21）：组合根 time_valid_fn 优先；未注入的回退
  // 用共享可信度下限（kPlausibleUnixFloor，防与展示层判定漂移）
  bool timeValid() const {
    if (self->cfg_.time_valid_fn) return self->cfg_.time_valid_fn();
    return nowUnix() >= kPlausibleUnixFloor;
  }

  // 任何 store 变更点（摄入/告警动作/适配器 runCycle/过期/静音规则）之后调用：
  // 摘要变了才 buildSnapshot + publish；force = 跳过比较（启动首帧/加载后）。
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

  // expire 巡检（TTL 到期 → Expired）+ 静音规则 TTL 落地（M21）：按
  // alert_expire_interval_ms 节拍；有记录过期或静音标志翻转即发布快照
  // （digest 判定会确认变化）。
  void expireDue(int64_t now) {
    if (!self->store_ || self->cfg_.alert_expire_interval_ms == 0) return;
    if (now < next_expire_ms_) return;
    next_expire_ms_ = now + self->cfg_.alert_expire_interval_ms;
    const uint64_t unix_now = nowUnix();
    const size_t n = self->store_->expire(unix_now);
    if (n > 0) {
      ESP_LOGI(TAG, "alert expire: %u record(s) expired", (unsigned)n);
    }
    // 以当前时间重估规则表：带 TTL 的规则到期后记录自动解除静音（store 语义
    // 保证仅标志翻转的记录才 touch/落库，无变化时零 db 写、digest 不变不发布）
    self->store_->setMuteRules(self->store_->muteRules(), unix_now);
    publishSnapshotIfChanged(false);
  }

  // alerts.db 压缩（store 唯一写方 = 本任务，压缩与追加天然串行）：按节拍检查
  // 文件大小与死行比例，超阈值即 tmp + 回读校验 + 替换。失败保留原文件，
  // 下个节拍重试。
  void maintainAlertDb(int64_t now) {
    if (!self->store_ || self->cfg_.alert_db_check_interval_ms == 0) return;
    if (now < next_db_check_ms_) return;
    next_db_check_ms_ = now + self->cfg_.alert_db_check_interval_ms;
    // 压缩流式进行，但仍需少量内部堆（单条序列化 + 文件缓冲）；堆紧张时本拍
    // 跳过，下个节拍再看，绝不在低内存下硬上。
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

  // ---- 命令处理（全部在 net_task 上下文）----
  void process(Command* cmd) {
    std::unique_ptr<Command> holder(cmd);  // 无论哪个分支都回收
    switch (cmd->type) {
      case CmdType::ReconnectNow: {
        auto p = std::static_pointer_cast<IdPayload>(cmd->payload);
        wifi_backoff_ms_ = kWifiBackoffStartMs;
        next_wifi_retry_ms_ = nowMs();
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
        publishSnapshotIfChanged(false);  // 摄入可能新建/更新记录（digest 判定）
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
              // 幂等：0 条可确认也是成功（affected=0），不视为错误
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
          // 全量替换 + 重估全部记录 muted 标志（store 语义）；规则变化即 store
          // 变更 → 快照发布（digest 判定，规则未命中任何记录时不重复发布）
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
        publishSnapshotIfChanged(true);  // 强制重建（store 可能在 net_task 外被加载）
        p->op.ok = true;
        p->op.give();
        break;
      }
      case CmdType::QueryStatus: {
        auto p = std::static_pointer_cast<StatusPayload>(cmd->payload);
        p->snapshot = NetStatusSnapshot{};  // 快照整体重建（payload 自有，天然无追加）
        p->snapshot.wifi.state = (int)self->wifi_.state();
        p->snapshot.wifi.ssid = self->wifi_.currentSsid();
        p->snapshot.wifi.rssi = self->wifi_.rssi();
        p->snapshot.wifi.ip = self->wifi_.currentIp();
        p->snapshot.wifi.last_reason = self->wifi_.lastReason();
        p->snapshot.wifi.reconnect_backoff_ms = wifi_backoff_ms_;
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
        // 统一时间源状态（M21）：time_valid_fn 判定可信性 + TimeSync SNTP 状态
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
        // 命令按序处理：执行到这里说明此前所有已入队工作完成
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

  // ---- WiFi 重连（指数退避，M13 F3）----
  void pumpWifi(int64_t now) {
    if (!self->cfg_.wifi_auto_reconnect) return;
    const bool provisioning_hold = self->wifiProvisioningHold();
    if (provisioning_hold) {
      wifi_provisioning_was_held_ = true;
      next_wifi_retry_ms_ = now + 1000;
      return;
    }
    if (wifi_provisioning_was_held_) {
      wifi_provisioning_was_held_ = false;
      wifi_backoff_ms_ = kWifiBackoffStartMs;
      next_wifi_retry_ms_ = 0;
    }
    WifiState st = self->wifi_.state();
    if (st == WifiState::Connected) {
      if (last_wifi_state_ != WifiState::Connected) {
        // 刚连上：退避复位 + SNTP（重）同步（M21）。dashboard 数据源由
        // data_pipeline 自行重试（离线退避到期即拉取）。
        wifi_backoff_ms_ = kWifiBackoffStartMs;
        next_wifi_retry_ms_ = 0;
        self->time_sync_.onWifiConnected();
      }
    } else if (st != WifiState::Connecting && now >= next_wifi_retry_ms_) {
      std::vector<WifiAccount> accounts;
      CredentialStore::loadWifiAccounts(accounts);
      if (accounts.empty()) {
        next_wifi_retry_ms_ = now + wifi_backoff_ms_;  // 未配网：按退避节奏空转
        return;
      }
      ESP_LOGI(TAG, "wifi reconnect attempt (backoff %ums)", (unsigned)wifi_backoff_ms_);
      self->wifi_.connectMulti(accounts);
      if (self->wifi_.state() != WifiState::Connected) {
        next_wifi_retry_ms_ = now + wifi_backoff_ms_;
        wifi_backoff_ms_ =
            std::min(wifi_backoff_ms_ * 2, self->cfg_.wifi_reconnect_backoff_max_ms);
      }
    }
    if (st != last_wifi_state_) {
      last_wifi_state_ = st;
      self->emit({NetEventType::WifiStateChanged, (int)st});
    }
  }

  // 距下一个事件点的毫秒数（上限 1000ms 保 housekeeping 精度）
  int64_t nextWakeMs(int64_t now) {
    int64_t next = now + 1000;
    if (next_wifi_retry_ms_ > 0 && next_wifi_retry_ms_ < next) next = next_wifi_retry_ms_;
    return std::max<int64_t>(next - now, 10);
  }

  static void taskTrampoline(void* arg) { static_cast<Impl*>(arg)->loop(); }

  void loop() {
    publishSnapshotIfChanged(true);  // 首帧快照（覆盖 start 前的 loadFrom 加载）
    self->emit({NetEventType::Started, 0});
    while (self->running_) {
      int64_t now = nowMs();
      // 先排空命令队列（每轮最多 16 条，防饿死轮询）
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
      self->time_sync_.service();  // SNTP 同步结果的 RTC 回写（net_task 上下文 I2C）
      if (self->pool_) self->pool_->housekeeping(now);
      // 阻塞等命令/到期（上限 1s 保 housekeeping 与事件及时性）
      TickType_t wait = pdMS_TO_TICKS((uint32_t)nextWakeMs(now));
      if (xQueueReceive(queue, &cmd, wait) == pdTRUE) process(cmd);
    }
    self->emit({NetEventType::Stopped, 0});
    task = nullptr;
    vTaskDelete(nullptr);
  }
};

// ---------------------------------------------------------------------------
// NetStack 公共接口
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

  // mbedTLS 走 PSRAM（≤4KB 内部 RAM）：TLS 握手/证书校验的必选项，
  // weread_app.cpp 实证不挂会握手 -0x7F00（M12 §4）。
  mbedtls_platform_set_calloc_free(
      [](size_t n, size_t size) -> void* {
        if (n * size <= 4096) {
          void* p = heap_caps_malloc(n * size, MALLOC_CAP_INTERNAL);
          if (p) {
            memset(p, 0, n * size);
            return p;
          }
        }
        void* p = heap_caps_malloc(n * size, MALLOC_CAP_SPIRAM);
        if (!p) p = heap_caps_malloc(n * size, MALLOC_CAP_INTERNAL);
        if (p) memset(p, 0, n * size);
        return p;
      },
      heap_caps_free);

  if (!impl_) impl_ = std::make_unique<Impl>();
  impl_->self = this;
  impl_->next_wifi_retry_ms_ = 0;
  impl_->wifi_backoff_ms_ = kWifiBackoffStartMs;
  impl_->last_wifi_state_ = WifiState::Disconnected;
  impl_->last_digest_ = 0;
  impl_->have_digest_ = false;
  impl_->next_expire_ms_ = 0;
  // 首次压缩检查推迟一个节拍：启动时组合根已按死行做过一次压缩。
  impl_->next_db_check_ms_ = Impl::nowMs() + cfg.alert_db_check_interval_ms;
  impl_->db_compactions_ = 0;
  impl_->db_last_compact_at_ = 0;
  time_sync_.configure(cfg.sntp_servers, cfg.rtc_write_fn);  // M21：SNTP + RTC 回写
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

// 命令发送 + 等待结果。payload 为共享负载（调用方保留引用直至返回）。
bool NetStack::sendCmdAndWait(CmdType type, const std::shared_ptr<void>& payload,
                              uint32_t wait_ms) {
  if (!running_ || !impl_ || !impl_->queue) return false;
  Impl::Command* cmd = new Impl::Command{type, payload};
  if (xQueueSend(impl_->queue, &cmd, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
    delete cmd;
    return false;
  }
  return true;  // 入队成功；结果经 payload->op 等待（由调用方决定等多久）
}

bool NetStack::reconnectNow(uint32_t wait_ms) {
  auto p = std::make_shared<Impl::IdPayload>();
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
  return p->result;  // take 成功后拷贝出负载（shared_ptr 保活，无悬垂）
}

AlertSnapshotSource NetStack::alertSnapshotSource() {
  alert_core::SnapshotPublisher* pub = cfg_.snapshot_publisher;
  // publisher 与 store 同生命周期契约（必须 outlive NetStack）；未接线时返回
  // nullptr 快照而非崩溃 —— admin 端点据此渲染空表/503（M17）。
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
  out = std::move(p->rules);  // take 成功后移出负载（shared_ptr 保活，无悬垂）
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
  *resp = std::move(p->resp);  // take 成功后才把结果移出负载
  return p->op.ok;
}

NetStack::PushResult NetStack::pushIngest(const std::string& body, const std::string& sender,
                                          uint32_t wait_ms) {
  PushResult r;
  // 批量预算（M11）：≤32 条 × 单条 ≤16KB；先卡总量再解析
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
