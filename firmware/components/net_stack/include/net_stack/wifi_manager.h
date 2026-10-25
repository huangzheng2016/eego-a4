// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// WiFi STA 状态机（改造自 eego-a4-weread wifi_manager，GPL-3.0）。
//
// 职责：STA 初始化、多账号依次连接、断线状态事件、IP/RSSI 查询、断电。
// 不负责：配网门户（admin 组件）、重连调度（net_task 按指数退避驱动 connectMulti）。
//
// 与 weread 版的差异：
//   * 凭据来自 NVS CredentialStore（weread 是 SD config.json 明文，M13 已废除）；
//   * 状态事件经 net_task 转发给 app_task（sys_evt 上下文不做重活）；
//   * 日志只打 SSID/原因码，绝不输出密码（M13 日志边界）。
//
// 省电默认 WIFI_PS_NONE（weread 真机结论：MIN_MODEM 在弱信号下 4-way handshake
// 超时/关联断开；常供电设备以连接稳定为先）。http_push 即时可达性不受影响——
// PS_NONE 只是不禁 modem sleep，STA 始终可收包。需要省电可 setPowerSave(true)。

#include <cstdint>
#include <string>
#include <vector>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "freertos/semphr.h"

#include "net_stack/credentials.h"
#include "net_stack/wifi_list.h"

namespace eego::net {

enum class WifiState : uint8_t {
  Disconnected = 0,  // 未连接（初始/主动断开）
  Connecting,        // 正在连接（关联+握手+DHCP）
  Connected,         // 已连接并拿到 IP
  Failed,            // 单次连接失败（原因见 lastReason）
  OfflineMode        // 全部账号失败（由调用方决定后续退避策略）
};

const char* wifiStateName(WifiState s);

// 快连提示（电池定时唤醒）：上次成功关联的 SSID + BSSID + 信道。带提示的首次连接
// 跳过全信道扫描与断开等待，直接按 BSSID/信道关联（单信道扫描 ~几十 ms）；失败
// 立即回退到常规 connectMulti（扫描 + 按名直连），提示作废。
struct ApHint {
  bool valid = false;
  std::string ssid;
  uint8_t bssid[6] = {0, 0, 0, 0, 0, 0};
  uint8_t channel = 0;
};

class WifiManager {
 public:
  static WifiManager& instance();

  // 初始化 esp_wifi（STA 模式）+ 默认事件循环 + 事件注册。幂等。
  esp_err_t init();

  // 多网络依次连接（列表顺序 = 优先级，见 wifi_list.h planWifiAttempts）：先扫描，
  // 可见的已知网络按顺序各试两次（30s/15s），未见网络（隐藏/漏检）随后各一次
  // （12s）；驱动报告 AP 不存在则跳过该网络剩余尝试。全部失败进入 OfflineMode，
  // 由 net_task 指数退避后重新遍历整表。按名直连不固定 BSSID
  // （weread 实证：固定 BSSID 在访客网络/虚拟多 BSSID 下反而认证失败）。
  // 例外：setFastConnectHint() 设置的一次性快连先按 BSSID + 信道尝试一次
  // （短超时），失败立即回到上述常规路径。
  WifiState connectMulti(const std::vector<WifiAccount>& accounts);

  // 连接指定账号（阻塞，含关联+握手+DHCP，timeout_ms 单次超时）。hint 非空且
  // valid 时固定 BSSID + 信道（快连）。
  WifiState connect(const WifiAccount& acc, uint32_t timeout_ms = 15000,
                    const ApHint* hint = nullptr);

  // 下一次 connectMulti 先按提示快连一次（一次性；须在连接前设置）。
  void setFastConnectHint(const ApHint& hint);
  // 最近一次成功关联的 AP（STA_CONNECTED 事件记录；无则 valid=false）。
  ApHint lastConnectedAp() const;

  // 主动断开当前连接（驱动保持运行；状态置 Disconnected，由 net_task 重连）。
  void disconnect();

  // 断开并停止驱动（电池唤醒流程结束前调用）。之后的 connect/connectMulti 会
  // 自动重新 esp_wifi_start()（不依赖重启）。
  void powerOff();

  // 长时间离线的恢复手段：esp_wifi_stop + esp_wifi_start（重新应用省电设置）。
  esp_err_t restartDriver();

  WifiState state() const { return state_; }
  bool isConnected() const { return state_ == WifiState::Connected; }
  int8_t rssi() const;
  std::string currentSsid() const { return current_ssid_; }
  std::string currentIp() const;
  uint8_t lastReason() const { return last_reason_; }
  // 最近一次 STA_DISCONNECTED 原因码（connect 开始时不清零，供状态/API 诊断）。
  uint8_t lastDisconnectReason() const { return last_disconnect_reason_; }
  uint32_t driverRestarts() const { return driver_restarts_; }

  // true → WIFI_PS_MIN_MODEM（省电，弱信号下可能不稳）；false → WIFI_PS_NONE。
  // 只在已连上（拿到 IP）后生效：关联/握手/DHCP 期间始终 WIFI_PS_NONE。
  void setPowerSave(bool enable);

  // ---- 离线诊断（任意任务可调用；内部加锁）----
  // 已存网络 + 每个网络最近一次尝试/结果/原因码 + 最近一次扫描。
  WifiDiagSnapshot diagnostics() const;
  // 立即扫描并记录（阻塞 ~2-3s）。正在连接（net_task 持有无线电）时返回 false，
  // 诊断里保留上一轮扫描结果。
  bool scanNow();
  // 同步诊断中的已存网络列表（启动/列表变更后调用；只用 SSID）。
  void syncSavedNetworks(const std::vector<WifiAccount>& accounts);
  // 一行摘要：mode/省电/国家码/快连提示（启动日志与串口 wifi 命令用）。
  std::string summary() const;
  bool fastHintPending() const { return fast_hint_.valid; }

  // AP 兜底是否到期（见 wifi_list.h ApFallbackPolicy：连续 3 轮失败 / 已存网络
  // 2 分钟都不可见 / 没有已存网络）。now_ms = esp_timer 毫秒。
  bool apFallbackDue(int64_t now_ms) const;
  // 最近一次扫描中信号最强的已存网络所在信道（0 = 都不可见/未扫描）。恢复热点用它
  // 作为 AP 信道：单射频下 AP 与 STA 同信道，STA 重试不必在两个信道间来回切。
  uint8_t bestSavedChannel() const;

 private:
  WifiManager();
  WifiState state_ = WifiState::Disconnected;
  bool initialized_ = false;
  bool started_ = false;  // esp_wifi_start 已生效（powerOff/restartDriver 会 stop）
  volatile uint8_t last_disconnect_reason_ = 0;
  uint32_t driver_restarts_ = 0;
  bool ps_enabled_ = false;
  std::string current_ssid_;
  uint8_t last_reason_ = 0;
  ApHint fast_hint_;
  ApHint last_ap_;

  EventGroupHandle_t event_group_ = nullptr;
  volatile bool connecting_ = false;
  bool fast_hint_used_ = false;  // 本次运行用过快连提示（诊断）

  // radio_mutex_（递归）：connectMulti/connect/scanNow 串行化，避免扫描与连接并发。
  SemaphoreHandle_t radio_mutex_ = nullptr;
  mutable SemaphoreHandle_t diag_mutex_ = nullptr;
  WifiDiagLog diag_;
  ApFallbackPolicy fallback_policy_;  // diag_mutex_ 保护

  void setState(WifiState s);
  void applyPowerSave();
  esp_err_t ensureStarted();
  bool scanLocked(std::vector<WifiScanHit>* hits);  // 调用方持 radio_mutex_
  void diagAttempt(const std::string& ssid);
  void diagResult(const std::string& ssid, WifiAttemptResult result, uint8_t reason);
  static void wifiEventHandler(void* arg, esp_event_base_t base, int32_t id, void* data);
};

}  // namespace eego::net
