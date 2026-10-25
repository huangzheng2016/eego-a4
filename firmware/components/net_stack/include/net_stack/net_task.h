// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// NetStack / net_task —— 网络栈单属主任务（M12 定稿并发模型）。
//
// net_task 是：
//   * 一切 esp_http_client 的唯一属主（HttpPool 只能在本任务上下文调用）；
//   * 一切 AlertStore 写入的唯一执行方（适配器产 AlertEvent，本任务 ingest；
//     告警动作 ack/ackAll/clear、expire 过期与静音规则 set/clear 同样只在
//     这里落地）；
//   * 快照发布方（注入 SnapshotPublisher 后，任何 store 变更——摄入/动作/过期/
//     加载/静音规则——都由本任务 buildSnapshot + publish；UI/API 无锁 acquire 读）；
//   * 出站 HTTP 执行方（dashboard data_pipeline 经 NetStackTransport 提交）；
//   * alerts.db 维护方（启动后按节拍检查大小/死行比例并压缩）；
//   * WiFi 重连状态机（指数退避 30s→60s→120s…封顶 5min，M13 F3）；
//   * 时间同步驱动（M21：WiFi 连上触发 SNTP，同步结果由本任务回写 RTC）。
//
// 其他任务（app_task/httpd/serial）不直接触碰本组件内部对象：
//   * 配置/查询/摄入/告警动作 → 命令队列（线程安全，调用方阻塞等结果，有超时）；
//   * WiFi/源状态变化 → NetEvent 回调（net_task 上下文，必须短平快）。
//
// 取数模型：dashboard 数据源（data_pipeline）是唯一的拉取路径；本组件不再
// 持有任何告警源轮询器。外部告警只经 POST /alerts 推送（pushIngest）、BLE
// observer 与串口注入（ingestExternal）进入 AlertStore，再由 DashboardRuntime
// 转为 dashboard 事件。
//
// 常供电与电池唤醒同一 API：
//   * 常供电：start 后 WiFi 常驻（modem-sleep），推送摄入即时可达；
//   * 电池唤醒：reconnectNow() → dashboard 拉取 → waitIdle() → powerOff。

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
  int wifi_state = 0;  // WifiState 枚举值
};

using NetEventCallback = void (*)(const NetEvent& ev, void* ctx);
using TimeFn = std::function<uint64_t()>;  // unix 秒（默认 time(nullptr)）
using TimeValidFn = std::function<bool()>;  // 统一时间源是否可信（M21）

struct NetStackConfig {
  PoolConfig pool;                          // ≤4 会话、闲置 30s、PSRAM 64K→512K
  bool wifi_auto_reconnect = true;          // 断线指数退避重连
  uint32_t wifi_reconnect_backoff_max_ms = kWifiBackoffMaxMs;  // 10s 起翻倍封顶 60s，永不放弃
  uint8_t task_priority = 5;                // M12 任务表：CPU0/5/16KB
  uint16_t task_stack_bytes = 16384;
  int8_t task_core = 0;                     // M12：net_task 固定 CPU0
  // 快照发布接线（M12：UI/API 无锁读快照，store 只能 net_task 写）。
  // 注入后，net_task 在任何 store 变更（摄入/ack/clear/过期/加载/静音规则）后
  // buildSnapshot + publish；nullptr = 不发布（alertSnapshotSource 恒返回
  // nullptr，动作不受影响）。必须 outlive NetStack（同 store/registry）。
  alert_core::SnapshotPublisher* snapshot_publisher = nullptr;
  // expire 过期巡检节拍（秒级 TTL 精度足够；0 = 关闭巡检）
  uint32_t alert_expire_interval_ms = 15000;
  // alerts.db 压缩检查节拍（0 = 关闭）与策略（大小阈值 / 死行比例）。
  uint32_t alert_db_check_interval_ms = 600000;
  // 已 attach 到 store 的 alerts.db（仅用于状态上报文件大小；可空 = 无 SD）。
  alert_core::AlertDb* alert_db = nullptr;
  alert_core::AlertStore::CompactPolicy alert_db_policy;
  // SNTP 服务器（空 = cn.pool.ntp.org 优先，按 CONFIG_LWIP_SNTP_MAX_SERVERS
  // 截断）；WiFi 连上即（重）同步。
  std::vector<std::string> sntp_servers;
  // RTC 回写钩子（M21：SNTP 同步成功后 net_task 把系统时间写进 PCF8563，
  // 电池唤醒/重启后离线时间保持可信）。可空 = 无 RTC，纯 SNTP。
  TimeSync::RtcWriteFn rtc_write_fn;
  // 统一时间源可信判定（M21：组合根注入 RTC 有效 || 系统时间可信的合并判定；
  // 空 = time_fn() > 0）。net_task 填 NetStatusSnapshot.time.valid，展示层
  // 据此如实显示「时间不可用」。
  TimeValidFn time_valid_fn;
};

class NetStack {
 public:
  static NetStack& instance();

  // 启动 net_task（幂等：已启动返回 ESP_ERR_INVALID_STATE）。
  // store/registry 必须 outlive NetStack；store 的全部写操作此后只发生在
  // net_task 上下文（M12 单属主模型）。time_fn 为空用 time(nullptr)。
  // cfg.snapshot_publisher 非空时，net_task 启动即发布首帧快照，此后任何
  // store 变更（摄入/告警动作/过期/加载后 republishSnapshot）都会重建发布。
  esp_err_t start(const NetStackConfig& cfg, alert_core::AlertStore* store,
                  const alert_core::TypeRegistry* registry, TimeFn time_fn = nullptr);
  void stop();

  bool running() const { return running_; }

  // 电池唤醒：立即重置 WiFi 退避并发起连接（不等待结果）
  // force=true：已连接时也先断开再按（新的）网络列表顺序重连（管理端删除/改密
  // 当前网络后使用）。
  bool reconnectNow(uint32_t wait_ms = 1000, bool force = false);
  // BLE wifi_prov 验证新凭据期间暂停自动重连，释放后立即恢复既有调度。
  void setWifiProvisioningHold(bool active) { wifi_provisioning_hold_.store(active); }
  bool wifiProvisioningHold() const { return wifi_provisioning_hold_.load(); }

  // ---- 查询（线程安全快照）----
  bool queryStatus(NetStatusSnapshot& out, uint32_t wait_ms = 1000);

  // ---- 告警动作（M11 ack/ack-all/clear；net_task 在 AlertStore 上下文执行）----
  // 线程安全：httpd/UI 线程经此入口提交，命令队列串行化，阻塞等结果（有超时）；
  // 绝不直写 store。结果语义见 alert_actions.h。
  AlertActionResult alertAction(const AlertAction& action, uint32_t wait_ms = 5000);

  // ---- 静音规则（M21；net_task 在 AlertStore 上下文执行并触发快照发布）----
  // set = 全量替换（store.setMuteRules 同语义：重估全部记录的 muted 标志）；
  // clear = 清空规则；query = 回当前规则表副本。支持 type/source/fingerprint
  // 三级匹配与 expires_at TTL（绝对 unix 秒，0 = 永不过期，重启后语义不变）。
  // 线程安全同告警动作：调用方阻塞等结果（有超时），绝不直写 store。
  bool setMuteRules(const std::vector<alert_core::MuteRule>& rules, std::string& err,
                    uint32_t wait_ms = 5000);
  bool clearMuteRules(std::string& err, uint32_t wait_ms = 5000);
  bool queryMuteRules(std::vector<alert_core::MuteRule>& out, uint32_t wait_ms = 1000);

  // 现成 functor，admin 直接注入 setAlertSnapshotSource(fn)：无锁 acquire
  // 当前快照；未接线/从未发布返回 nullptr（admin 渲染空表/503）。
  AlertSnapshotSource alertSnapshotSource();
  // 现成 functor，admin 直接注入 setAlertActionSink(fn)：net_task 执行并回
  // 明确结果；net_task 未运行时 Unavailable + detail（admin 映射 503）。
  AlertActionSink alertActionSink();
  // store 在 net_task 之外变更后（如组合根加载 alerts.db 完成），请求 net_task
  // 重建并发布快照。正常接线顺序（先 load 后 start）无需调用——net_task 启动
  // 时会发布首帧快照。
  bool republishSnapshot(uint32_t wait_ms = 1000);

  // ---- 通用出站（dashboard data_pipeline / sources/test；net_task 执行）----
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

  // ---- 外部摄入统一入口（serial inject / ble_scan observer）----
  // 事件已经 alert_core::eventsFromJsonText 或等价校验；net_task 执行 ingest。
  bool ingestExternal(const std::vector<alert_core::AlertEvent>& events,
                      const alert_core::AlertSource& source, uint32_t wait_ms = 5000);

  // ---- 事件通知（net_task 上下文回调，必须短平快）----
  void setEventCallback(NetEventCallback cb, void* ctx);

  // ---- 电池唤醒：等待所有已入队工作完成（网络静默）----
  bool waitIdle(uint32_t wait_ms = 30000);

  WifiManager& wifi() { return wifi_; }
  const NetStackConfig& config() const { return cfg_; }

 private:
  NetStack();
  ~NetStack();
  NetStack(const NetStack&) = delete;
  NetStack& operator=(const NetStack&) = delete;

  // 命令类型（命令队列模型；负载为 shared_ptr<void>，net_task 处理完释放）
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
  std::unique_ptr<HttpPool> pool_;  // 仅 net_task 上下文使用（单属主）

  NetStackConfig cfg_;
  WifiManager& wifi_;
  alert_core::AlertStore* store_ = nullptr;
  const alert_core::TypeRegistry* registry_ = nullptr;
  TimeFn time_fn_;
  TimeSync time_sync_;  // net_task 上下文驱动（M21：WiFi 连上 SNTP 同步 + RTC 回写）
  NetEventCallback event_cb_ = nullptr;
  void* event_ctx_ = nullptr;
  bool running_ = false;
  std::atomic<bool> wifi_provisioning_hold_{false};

  // 命令入队（payload 共享所有权，net_task 处理完才释放最后引用）。
  bool sendCmdAndWait(CmdType type, const std::shared_ptr<void>& payload, uint32_t wait_ms);
  void emit(const NetEvent& ev);

  friend struct Impl;
};

}  // namespace eego::net
