// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// NetStack / net_task —— 网络栈单属主任务（M12 定稿并发模型）。
//
// net_task 是：
//   * 一切 esp_http_client 的唯一属主（HttpPool 只能在本任务上下文调用）；
//   * 一切 AlertStore 写入的唯一执行方（适配器产 AlertEvent，本任务 ingest）；
//   * 轮询调度器（每源 interval，vTaskDelay 级节拍，指数退避）；
//   * WiFi 重连状态机（指数退避 30s→60s→120s…封顶 5min，M13 F3）。
//
// 其他任务（app_task/httpd/serial）不直接触碰本组件内部对象：
//   * 配置/查询/摄入 → 命令队列（线程安全，调用方阻塞等结果，有超时）；
//   * WiFi/源状态变化 → NetEvent 回调（net_task 上下文，必须短平快）。
//
// 常供电与电池唤醒同一 API：
//   * 常供电：start 后自动轮询 + WiFi 常驻；
//   * 电池唤醒：reconnectNow() → pollNow("") → waitIdle() → 读结果 → powerOff。
//     轮询/缓存/清除语义完全一致（cache/<source>.json 离线缓存两模式共用）。

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "alert_core/store.h"
#include "alert_core/type_registry.h"
#include "esp_err.h"
#include "net_stack/http_pool.h"
#include "net_stack/source_adapter.h"
#include "net_stack/source_cache.h"
#include "net_stack/source_config.h"
#include "net_stack/source_health.h"
#include "net_stack/wifi_manager.h"

namespace eego::net {

enum class NetEventType : uint8_t {
  Started = 0,
  Stopped,
  WifiStateChanged,
  SourceHealthChanged,  // 源在 成功↔失败 之间跃迁（状态页可见性，M11 验收 2）
};

struct NetEvent {
  NetEventType type = NetEventType::Started;
  int wifi_state = 0;     // WifiState 枚举值
  std::string source_id;  // SourceHealthChanged 时有效
};

using NetEventCallback = void (*)(const NetEvent& ev, void* ctx);
using TimeFn = std::function<uint64_t()>;  // unix 秒（默认 time(nullptr)）

struct NetStackConfig {
  std::string cache_dir = "/sdcard/alertpanel/cache";
  PoolConfig pool;                          // ≤4 会话、闲置 30s、PSRAM 64K→512K
  bool wifi_auto_reconnect = true;          // 断线指数退避重连
  uint32_t wifi_reconnect_backoff_max_ms = 300000;  // M13 F3：30s 起翻倍封顶 5min
  uint32_t source_backoff_max_ms = 1800000;         // 源级退避封顶（interval<<n）
  uint8_t task_priority = 5;                // M12 任务表：CPU0/5/16KB
  uint16_t task_stack_bytes = 16384;
  int8_t task_core = 0;                     // M12：net_task 固定 CPU0
};

class NetStack {
 public:
  static NetStack& instance();

  // 启动 net_task（幂等：已启动返回 ESP_ERR_INVALID_STATE）。
  // store/registry 必须 outlive NetStack；store 的全部写操作此后只发生在
  // net_task 上下文（M12 单属主模型）。time_fn 为空用 time(nullptr)。
  esp_err_t start(const NetStackConfig& cfg, alert_core::AlertStore* store,
                  const alert_core::TypeRegistry* registry, TimeFn time_fn = nullptr);
  void stop();

  bool running() const { return running_; }

  // ---- 源管理（线程安全；net_task 执行，调用方阻塞至完成或超时）----
  bool addSource(const SourceConfig& cfg, std::string& err, uint32_t wait_ms = 5000);
  bool removeSource(const std::string& id, uint32_t wait_ms = 5000);
  bool setSourceEnabled(const std::string& id, bool enabled, uint32_t wait_ms = 5000);
  // 立即轮询（"" = 全部源）。常供电即时刷新与电池唤醒批量拉取同一入口。
  bool pollNow(const std::string& id = "", uint32_t wait_ms = 5000);
  // 电池唤醒：立即重置 WiFi 退避并发起连接（不等待结果）
  bool reconnectNow(uint32_t wait_ms = 1000);

  // ---- 查询（线程安全快照）----
  bool queryStatus(NetStatusSnapshot& out, uint32_t wait_ms = 1000);

  // ---- 通用出站（admin sources/test 等；net_task 执行）----
  // resp 由调用方持有（move-only）；false = 入队/等待失败。
  bool httpSubmit(const NetRequest& req, NetResponse* resp, uint32_t wait_ms = 10000);

  // ---- http_push 摄入桥接（admin httpd 上下文调用）----
  struct PushResult {
    enum class Code {
      Ok = 0,     // 全部摄入成功（201/202 由 admin 按 created/updated 细分）
      BadRequest, // 解析/校验失败（error 为字段级错误，admin 映射 400）
      Oversize,   // body 超批量预算（admin 映射 413）
      Unavailable, // 队列满/net_task 未运行/等待超时（admin 映射 503）
      StoreFull,  // 全部因 store_full 被拒（admin 映射 503）
    } code = Code::Unavailable;
    int created = 0;
    int updated = 0;
    int rejected = 0;
    alert_core::ValidationError error;
  };
  PushResult pushIngest(const std::string& body, const std::string& sender,
                        uint32_t wait_ms = 5000);

  // ---- 外部摄入统一入口（serial inject / ble_scan 预留注册点）----
  // 事件已经 alert_core::eventsFromJsonText 或等价校验；net_task 执行 ingest。
  bool ingestExternal(const std::vector<alert_core::AlertEvent>& events,
                      const alert_core::AlertSource& source, uint32_t wait_ms = 5000);

  // ---- 事件通知（net_task 上下文回调，必须短平快）----
  void setEventCallback(NetEventCallback cb, void* ctx);

  // ---- 电池唤醒：等待所有已入队工作完成（网络静默）----
  bool waitIdle(uint32_t wait_ms = 30000);

  WifiManager& wifi() { return wifi_; }
  const NetStackConfig& config() const { return cfg_; }
  SourceCache& cache() { return cache_; }

 private:
  NetStack();
  ~NetStack();
  NetStack(const NetStack&) = delete;
  NetStack& operator=(const NetStack&) = delete;

  // 命令类型（命令队列模型；负载为 shared_ptr<void>，net_task 处理完释放）
  enum class CmdType : int {
    AddSource,
    RemoveSource,
    SetEnabled,
    PollNow,
    ReconnectNow,
    HttpSubmit,
    Ingest,
    QueryStatus,
    WaitIdle,
    Stop,
  };

  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::unique_ptr<HttpPool> pool_;  // 仅 net_task 上下文使用（单属主）

  NetStackConfig cfg_;
  WifiManager& wifi_;
  SourceCache cache_;
  alert_core::AlertStore* store_ = nullptr;
  const alert_core::TypeRegistry* registry_ = nullptr;
  TimeFn time_fn_;
  NetEventCallback event_cb_ = nullptr;
  void* event_ctx_ = nullptr;
  bool running_ = false;

  // 命令入队（payload 共享所有权，net_task 处理完才释放最后引用）。
  bool sendCmdAndWait(CmdType type, const std::shared_ptr<void>& payload, uint32_t wait_ms);
  void emit(const NetEvent& ev);

  friend struct Impl;
};

}  // namespace eego::net
