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
// 超时/关联断开；常供电设备以连接稳定为先）。组合根只在电池交互窗口调用
// setPowerSave(true)（见 wifiModemSleepWanted）；常供电/保持唤醒始终 PS_NONE。
//
// 弱信号加固（每次 esp_wifi_start 后 applyRadioTuning 重新应用）：STA/AP 固定
// HT20、STA beacon 丢失容忍 kWifiStaInactiveS 秒、按需修正被压低的最大发射功率
// 并记录生效值；beacon 超时断线先按原 BSSID/信道快速重连（不做整表扫描）；
// 已关联但 DHCP 慢时把单次尝试截止延到关联后 30s；持续弱信号时在同名 BSSID 间
// 简单漫游（wifi_list.h WifiRoamPolicy）。墨水屏刷新（面板高压）期间推迟扫描/关联。

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

  // 重新应用射频调优（HT20 / beacon 容忍 / 省电 / 发射功率检查）。esp_wifi_start
  // 或切换 AP/APSTA 模式后调用（WifiManager 内部的 start 已自动调用）。
  void applyRadioTuning();

  // ---- 链路质量（net_task 在已连接时每轮调用）----
  // 按 kWifiLinkSampleMs 采样 RSSI；返回 true = 漫游扫描到期（调用方随后 roam()）。
  bool serviceLink(int64_t now_ms);
  // 同名扫描 + 选更强 BSSID（≥8 dB）并切换。accounts 用于取当前 SSID 的密码。
  // 返回 true = 已切到新 BSSID；无更好目标/扫描失败返回 false（链路保持）。
  bool roam(const std::vector<WifiAccount>& accounts);

  struct LinkInfo {
    WifiLinkSummary rssi;           // 近 10 分钟 RSSI min/avg/last
    uint32_t beacon_timeouts = 0;   // WIFI_EVENT_STA_BEACON_TIMEOUT 累计
    uint32_t roams = 0;             // 成功漫游次数
    uint32_t roam_scans = 0;        // 漫游评估扫描次数
    int32_t last_assoc_ms = -1;     // 最近一次连接：开始 → 关联（-1 = 无）
    int32_t last_dhcp_ms = -1;      // 最近一次连接：关联 → 拿到 IP（-1 = 无）
    int8_t tx_power_qdbm = 0;       // 生效的最大发射功率（0.25 dBm；0 = 未知）
  };
  LinkInfo linkInfo() const;

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

  // 链路统计/漫游（diag_mutex_ 保护）
  WifiLinkStats link_stats_;
  WifiRoamPolicy roam_policy_;
  volatile uint32_t beacon_timeouts_ = 0;
  uint32_t roams_ = 0;
  uint32_t roam_scans_ = 0;
  int32_t last_assoc_ms_ = -1;
  int32_t last_dhcp_ms_ = -1;
  int8_t tx_power_qdbm_ = 0;
  volatile uint32_t assoc_at_ms_ = 0;     // STA_CONNECTED 时刻（esp_timer ms 低 32 位）
  volatile bool relink_pending_ = false;  // beacon 超时断线：下一轮先按原 BSSID 快连

  void setState(WifiState s);
  void applyPowerSave();
  void checkTxPower(const char* when);
  void waitRfQuiet(const char* what);
  esp_err_t ensureStarted();
  bool scanLocked(std::vector<WifiScanHit>* hits);  // 调用方持 radio_mutex_
  void diagAttempt(const std::string& ssid);
  void diagResult(const std::string& ssid, WifiAttemptResult result, uint8_t reason);
  static void wifiEventHandler(void* arg, esp_event_base_t base, int32_t id, void* data);
};

}  // namespace eego::net
