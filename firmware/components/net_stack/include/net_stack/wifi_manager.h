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

#include "net_stack/credentials.h"

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

  // 多账号依次连接：每个 SSID 最多 3 次（首次 30s、后续 15s，弱信号容忍），
  // 全部失败进入 OfflineMode。扫描仅作诊断提示，按名直连不固定 BSSID
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

  // 断开并停止驱动（电池唤醒流程结束前调用）
  void powerOff();

  WifiState state() const { return state_; }
  bool isConnected() const { return state_ == WifiState::Connected; }
  int8_t rssi() const;
  std::string currentSsid() const { return current_ssid_; }
  std::string currentIp() const;
  uint8_t lastReason() const { return last_reason_; }

  // true → WIFI_PS_MIN_MODEM（省电，弱信号下可能不稳）；false → WIFI_PS_NONE。
  // 连接前/重连前调用生效；连接中切换立即应用。
  void setPowerSave(bool enable);

 private:
  WifiManager() = default;
  WifiState state_ = WifiState::Disconnected;
  bool initialized_ = false;
  bool ps_enabled_ = false;
  std::string current_ssid_;
  uint8_t last_reason_ = 0;
  ApHint fast_hint_;
  ApHint last_ap_;

  EventGroupHandle_t event_group_ = nullptr;
  volatile bool connecting_ = false;

  void setState(WifiState s);
  void applyPowerSave();
  static void wifiEventHandler(void* arg, esp_event_base_t base, int32_t id, void* data);
};

}  // namespace eego::net
