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

#include "net_stack/http_poll_adapter.h"

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

  struct AddSrcPayload {
    SourceConfig cfg;
    PendingOp op;
  };
  struct IdPayload {  // RemoveSource / SetEnabled / PollNow / ReconnectNow
    std::string id;
    bool flag = false;  // SetEnabled: 目标状态；PollNow/ReconnectNow 忽略
    PendingOp op;
  };
  struct HttpPayload {
    NetRequest req;
    NetResponse* resp = nullptr;  // 调用方持有
    PendingOp op;
  };
  struct IngestPayload {
    std::vector<alert_core::AlertEvent> events;
    alert_core::AlertSource source;
    std::vector<alert_core::IngestOutcome> outcomes;  // net_task 回填
    PendingOp op;
  };
  struct StatusPayload {
    NetStatusSnapshot* out = nullptr;
    PendingOp op;
  };
  struct SemPayload {  // WaitIdle / Stop
    PendingOp op;
  };

  struct SourceRuntime {
    SourceConfig cfg;
    std::unique_ptr<ISourceAdapter> adapter;
    SourceHealth health;
    int64_t next_due_ms = 0;  // esp_timer 毫秒
    bool last_ok = true;      // 上一次轮询成败（跃迁检测用）
  };

  NetStack* self = nullptr;
  QueueHandle_t queue = nullptr;
  TaskHandle_t task = nullptr;
  std::vector<std::unique_ptr<SourceRuntime>> sources_;

  // WiFi 重连状态（net_task 上下文私有）
  int64_t next_wifi_retry_ms_ = 0;
  uint32_t wifi_backoff_ms_ = kWifiBackoffStartMs;
  WifiState last_wifi_state_ = WifiState::Disconnected;

  static int64_t nowMs() { return esp_timer_get_time() / 1000; }
  uint64_t nowUnix() const {
    return self->time_fn_ ? self->time_fn_() : (uint64_t)time(nullptr);
  }

  SourceRuntime* findSource(const std::string& id) {
    for (auto& s : sources_)
      if (s->cfg.id == id) return s.get();
    return nullptr;
  }

  // ---- 命令处理（全部在 net_task 上下文）----
  void process(Command* cmd) {
    std::unique_ptr<Command> holder(cmd);  // 无论哪个分支都回收
    switch (cmd->type) {
      case CmdType::AddSource: {
        auto p = std::static_pointer_cast<AddSrcPayload>(cmd->payload);
        if (findSource(p->cfg.id)) {
          p->op.ok = false;
          p->op.err = "source id already exists";
        } else {
          auto rt = std::make_unique<SourceRuntime>();
          rt->cfg = p->cfg;
          rt->adapter = SourceAdapterRegistry::instance().create(p->cfg.kind);
          if (!rt->adapter) {
            p->op.ok = false;
            p->op.err = "unknown source kind: " + p->cfg.kind;
          } else {
            rt->health.id = p->cfg.id;
            rt->health.kind = p->cfg.kind;
            rt->health.enabled = p->cfg.enabled;
            rt->health.interval_ms = p->cfg.interval_ms;
            rt->next_due_ms = nowMs();  // 新源立即首轮（M11 验收 2：≤interval+5s）
            sources_.push_back(std::move(rt));
            p->op.ok = true;
          }
        }
        p->op.give();
        break;
      }
      case CmdType::RemoveSource: {
        auto p = std::static_pointer_cast<IdPayload>(cmd->payload);
        for (auto it = sources_.begin(); it != sources_.end(); ++it) {
          if ((*it)->cfg.id == p->id) {
            sources_.erase(it);
            p->op.ok = true;
            break;
          }
        }
        if (!p->op.ok) p->op.err = "source not found";
        p->op.give();
        break;
      }
      case CmdType::SetEnabled: {
        auto p = std::static_pointer_cast<IdPayload>(cmd->payload);
        SourceRuntime* rt = findSource(p->id);
        if (!rt) {
          p->op.err = "source not found";
        } else {
          rt->cfg.enabled = p->flag;
          rt->health.enabled = p->flag;
          if (p->flag) rt->next_due_ms = nowMs();  // 重新启用立即轮询
          p->op.ok = true;
        }
        p->op.give();
        break;
      }
      case CmdType::PollNow: {
        auto p = std::static_pointer_cast<IdPayload>(cmd->payload);
        if (p->id.empty()) {
          for (auto& s : sources_)
            if (s->cfg.enabled) s->next_due_ms = nowMs();
        } else {
          SourceRuntime* rt = findSource(p->id);
          if (rt && rt->cfg.enabled) rt->next_due_ms = nowMs();
        }
        p->op.ok = true;
        p->op.give();
        break;
      }
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
        if (p->resp && self->pool_) *p->resp = self->pool_->execute(p->req);
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
        p->op.give();
        break;
      }
      case CmdType::QueryStatus: {
        auto p = std::static_pointer_cast<StatusPayload>(cmd->payload);
        if (p->out) {
          p->out->wifi.state = (int)self->wifi_.state();
          p->out->wifi.ssid = self->wifi_.currentSsid();
          p->out->wifi.rssi = self->wifi_.rssi();
          p->out->wifi.ip = self->wifi_.currentIp();
          p->out->wifi.last_reason = self->wifi_.lastReason();
          p->out->wifi.reconnect_backoff_ms = wifi_backoff_ms_;
          p->out->pool_sessions = self->pool_ ? self->pool_->sessionCount() : 0;
          p->out->running = true;
          for (const auto& s : sources_) p->out->sources.push_back(s->health);
        }
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
    WifiState st = self->wifi_.state();
    if (st == WifiState::Connected) {
      if (last_wifi_state_ != WifiState::Connected) {
        // 刚连上：退避复位 + 全部源立即重轮
        wifi_backoff_ms_ = kWifiBackoffStartMs;
        next_wifi_retry_ms_ = 0;
        for (auto& s : sources_) {
          if (s->cfg.enabled && s->next_due_ms > now) s->next_due_ms = now;
        }
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
      self->emit({NetEventType::WifiStateChanged, (int)st, {}});
    }
  }

  // ---- 到期源轮询（源级指数退避）----
  void pumpSources(int64_t now) {
    for (auto& s : sources_) {
      if (!s->cfg.enabled || now < s->next_due_ms) continue;
      AdapterContext ctx;
      ctx.pool = self->pool_.get();
      ctx.store = self->store_;
      ctx.registry = self->registry_;
      ctx.cache = &self->cache_;
      ctx.now = nowUnix();

      bool ok = s->adapter->runCycle(ctx, s->cfg, s->health);
      if (ok) {
        s->health.consecutive_failures = 0;
        s->health.backoff_level = 0;
        s->next_due_ms = now + s->cfg.interval_ms;
      } else {
        s->health.consecutive_failures++;
        s->health.backoff_level = std::min(s->health.backoff_level + 1, (uint32_t)6);
        uint32_t backoff = std::min(s->cfg.interval_ms << s->health.backoff_level,
                                    self->cfg_.source_backoff_max_ms);
        s->next_due_ms = now + backoff;
        ESP_LOGW(TAG, "source %s poll failed (#%u, next in %ums): %s", s->cfg.id.c_str(),
                 (unsigned)s->health.consecutive_failures, (unsigned)backoff,
                 s->health.last_error.c_str());
      }
      if (ok != s->last_ok) {
        s->last_ok = ok;
        self->emit({NetEventType::SourceHealthChanged, (int)self->wifi_.state(), s->cfg.id});
      }
    }
  }

  // 距下一个事件点的毫秒数（上限 1000ms 保 housekeeping 精度）
  int64_t nextWakeMs(int64_t now) {
    int64_t next = now + 1000;
    for (const auto& s : sources_) {
      if (s->cfg.enabled && s->next_due_ms < next) next = s->next_due_ms;
    }
    if (next_wifi_retry_ms_ > 0 && next_wifi_retry_ms_ < next) next = next_wifi_retry_ms_;
    return std::max<int64_t>(next - now, 10);
  }

  static void taskTrampoline(void* arg) { static_cast<Impl*>(arg)->loop(); }

  void loop() {
    self->emit({NetEventType::Started, 0, {}});
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
      pumpSources(now);
      if (self->pool_) self->pool_->housekeeping(now);
      // 阻塞等命令/到期（上限 1s 保 housekeeping 与事件及时性）
      TickType_t wait = pdMS_TO_TICKS((uint32_t)nextWakeMs(now));
      if (xQueueReceive(queue, &cmd, wait) == pdTRUE) process(cmd);
    }
    self->emit({NetEventType::Stopped, 0, {}});
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

NetStack::NetStack() : wifi_(WifiManager::instance()), cache_("/sdcard/alertpanel/cache") {}

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
  cache_ = SourceCache(cfg.cache_dir);
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
  impl_->sources_.clear();
  impl_->next_wifi_retry_ms_ = 0;
  impl_->wifi_backoff_ms_ = kWifiBackoffStartMs;
  impl_->last_wifi_state_ = WifiState::Disconnected;
  if (!impl_->queue) impl_->queue = xQueueCreate(16, sizeof(Impl::Command*));
  if (!impl_->queue) return ESP_ERR_NO_MEM;

  // 内置适配器（http_poll）；ble_scan/web_chart/serial inject 后装在注册点注册
  registerHttpPollAdapter(SourceAdapterRegistry::instance());

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

bool NetStack::addSource(const SourceConfig& cfg, std::string& err, uint32_t wait_ms) {
  auto p = std::make_shared<Impl::AddSrcPayload>();
  p->cfg = cfg;
  if (!sendCmdAndWait(CmdType::AddSource, p, wait_ms) ||
      xSemaphoreTake(p->op.sem, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
    err = "net_task busy/unavailable";
    return false;
  }
  err = p->op.err;
  return p->op.ok;
}

bool NetStack::removeSource(const std::string& id, uint32_t wait_ms) {
  auto p = std::make_shared<Impl::IdPayload>();
  p->id = id;
  if (!sendCmdAndWait(CmdType::RemoveSource, p, wait_ms) ||
      xSemaphoreTake(p->op.sem, pdMS_TO_TICKS(wait_ms)) != pdTRUE)
    return false;
  return p->op.ok;
}

bool NetStack::setSourceEnabled(const std::string& id, bool enabled, uint32_t wait_ms) {
  auto p = std::make_shared<Impl::IdPayload>();
  p->id = id;
  p->flag = enabled;
  if (!sendCmdAndWait(CmdType::SetEnabled, p, wait_ms) ||
      xSemaphoreTake(p->op.sem, pdMS_TO_TICKS(wait_ms)) != pdTRUE)
    return false;
  return p->op.ok;
}

bool NetStack::pollNow(const std::string& id, uint32_t wait_ms) {
  auto p = std::make_shared<Impl::IdPayload>();
  p->id = id;
  if (!sendCmdAndWait(CmdType::PollNow, p, wait_ms) ||
      xSemaphoreTake(p->op.sem, pdMS_TO_TICKS(wait_ms)) != pdTRUE)
    return false;
  return p->op.ok;
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
  p->out = &out;
  if (!sendCmdAndWait(CmdType::QueryStatus, p, wait_ms) ||
      xSemaphoreTake(p->op.sem, pdMS_TO_TICKS(wait_ms)) != pdTRUE)
    return false;
  return p->op.ok;
}

bool NetStack::httpSubmit(const NetRequest& req, NetResponse* resp, uint32_t wait_ms) {
  if (!resp) return false;
  auto p = std::make_shared<Impl::HttpPayload>();
  p->req = req;
  p->resp = resp;
  if (!sendCmdAndWait(CmdType::HttpSubmit, p, wait_ms) ||
      xSemaphoreTake(p->op.sem, pdMS_TO_TICKS(wait_ms)) != pdTRUE)
    return false;
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
