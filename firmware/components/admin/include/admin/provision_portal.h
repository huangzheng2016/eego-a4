// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// 首次配网门户 + AP 兜底（M13 F1/F2/T8 定稿）。
//
// 配网门户（INIT_PROVISION 模式，仅首启无管理密码时由 app_task 开启）：
//   * 开放 AP「EEGO-A4-Setup」（固定 SSID 不含设备唯一标识）+ APSTA；
//   * DNS 劫持 captive portal + 门户 httpd（80 端口；与管理 API 互斥，
//     两者由 app_task 按模式保证不会同时运行，本组件防御性拒绝双开）；
//   * 门户窗口 10 分钟无操作自动关闭（任何门户请求重置空闲计时）；
//   * 配网页两步：选 WiFi 输密码 → 强制设管理密码（≥8 位）；
//   * 提交后 APSTA 试连 STA（15s 超时，中文原因码）→ 成功写 NVS
//     （WiFi 凭据 + PBKDF2 hash + 生成 ingest key）→ 关门户 → 重启。
//
// AP 兜底（AP_FALLBACK 模式，手动触发；auto_ap_fallback 判定在 app_task）：
//   * 同一开放 AP + 10 分钟固定窗口（无活动重置），窗口到自动关；
//   * 兜底 AP 上的管理 API 仍要完整认证（管理 API 绑 0.0.0.0 天然可达）。
//
// 安全边界：门户开放期内任何人可连 AP 提交配网——窗口化即首版控制
// （M13 T8）；门户 HTTP 无任何认证（首启还没有密码），配网成功即设密。

#include <cstdint>
#include <string>

#include "esp_err.h"
#include "esp_event.h"

namespace eego::admin {

class ProvisionPortal {
 public:
  // 恢复/配网热点的固定 SSID 与门户地址（设备引导页、状态页展示用）。
  static constexpr const char* kApSsid = "EEGO-A4-Setup";
  static constexpr const char* kApUrl = "http://192.168.4.1/";

  static ProvisionPortal& instance();

  // ---- 配网门户（INIT_PROVISION）----
  esp_err_t open();               // 已开启返回 ESP_ERR_INVALID_STATE
  void close(const char* reason);  // "expired" | "success" | "manual"
  bool isOpen() const { return portal_running_; }
  uint32_t windowRemainingMs() const;

  // ---- AP 兜底（AP_FALLBACK）----
  esp_err_t openApFallback();
  void closeApFallback(const char* reason);
  bool isApFallbackActive() const { return fallback_running_; }

  // AP 是否活跃（门户或兜底）
  bool apActive() const { return portal_running_ || fallback_running_; }

  // 门户 HTTP 活动上报（httpd handler 调用，重置 10 分钟空闲窗口）
  void notePortalActivity();

  // 配网状态机快照（/status 用）
  enum class ProvState : uint8_t { Idle = 0, Connecting, Ok, Fail };
  ProvState provState() const { return prov_state_; }
  const std::string& provReason() const { return prov_reason_; }
  const std::string& provIp() const { return prov_ip_; }

  // 内部：STA 事件处理（配网期间注册；与 WifiManager 并存互不干扰）
  static void wifiEventHandler(void* arg, esp_event_base_t base, int32_t id, void* data);

  // ---- 配网状态机（门户 httpd handler 与 sys_evt 回调直接读写）----
  // 并发说明：写方只有门户 httpd 任务（/connect）与 sys_evt 事件回调；
  // 读方为门户 httpd（/status）。竞态有界（最多一次轮询内状态滞后），
  // v1 不加锁；所有字符串写入均为整体赋值。
  ProvState prov_state_ = ProvState::Idle;
  std::string prov_reason_;
  std::string prov_ip_;
  std::string prov_ssid_;
  std::string prov_wifi_pass_;   // 配网期间暂存，成功/关闭即清除，绝不落日志/SD
  std::string prov_admin_pass_;  // 仅在配网成功路径使用，绝不落日志/SD
  int64_t connect_start_us_ = 0;

  void checkConnectTimeout();
  void onConnectSucceeded(const std::string& ip);
  void onConnectFailed(const char* reason);

 private:
  ProvisionPortal() = default;

  bool portal_running_ = false;
  bool fallback_running_ = false;
  bool ap_netif_created_ = false;
  bool sta_handler_registered_ = false;

  void* window_timer_ = nullptr;  // esp_timer_handle_t
  int64_t window_deadline_ms_ = 0;  // 兜底固定窗口截止（esp_timer 毫秒）

  esp_err_t startAp();   // 建 AP netif + APSTA + 配置开放 AP
  void stopAp();         // 回 STA 模式
  void startWindowTimer(bool idle_reset);
  void stopWindowTimer();
};

}  // namespace eego::admin
