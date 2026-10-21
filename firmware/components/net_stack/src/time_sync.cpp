// SPDX-License-Identifier: GPL-3.0-or-later
// TimeSync 实现 —— 见 include/net_stack/time_sync.h。
#include "net_stack/time_sync.h"

#include <algorithm>
#include <ctime>

#include "esp_log.h"
#include "esp_sntp.h"
#include "sdkconfig.h"

namespace eego::net {

namespace {

const char* TAG = "net.time";

// SNTP 同步通知回调（lwIP TCPIP 上下文）：只做原子置位，绝不碰 I2C/RTC。
// IDF 协议栈在进入本回调前已 settimeofday 设置系统时间。
// NetStack 单例 ⇒ TimeSync 实例唯一，静态指针安全。
TimeSync* g_instance = nullptr;

void onSntpSync(struct timeval* tv) {
  if (g_instance == nullptr || tv == nullptr) return;
  g_instance->noteSync(static_cast<uint64_t>(tv->tv_sec));
}

}  // namespace

void TimeSync::configure(std::vector<std::string> servers, RtcWriteFn rtc_write) {
  servers_ = std::move(servers);
  rtc_write_ = std::move(rtc_write);
  g_instance = this;
}

void TimeSync::onWifiConnected() {
  if (servers_.empty()) {
    servers_ = {"cn.pool.ntp.org", "pool.ntp.org"};
  }
  // esp_sntp 服务器槽上限 = CONFIG_LWIP_SNTP_MAX_SERVERS（本工程 sdkconfig 为 1），
  // 多配截断防越界断言
  const size_t n = std::min(servers_.size(),
                            static_cast<size_t>(CONFIG_LWIP_SNTP_MAX_SERVERS));
  if (!sntp_initialized_) {
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    for (size_t i = 0; i < n; i++) {
      esp_sntp_setservername(static_cast<u8_t>(i), servers_[i].c_str());
    }
    esp_sntp_set_time_sync_notification_cb(&onSntpSync);
    esp_sntp_init();
    sntp_initialized_ = true;
    ESP_LOGI(TAG, "sntp started (server %s)", servers_[0].c_str());
    return;
  }
  // 重连：stop + init 立即重新同步（不等每小时自同步下一拍）
  esp_sntp_stop();
  esp_sntp_init();
  ESP_LOGI(TAG, "sntp re-sync requested");
}

void TimeSync::service() {
  if (!rtc_write_pending_.exchange(false, std::memory_order_relaxed)) return;
  if (!rtc_write_) return;  // 无 RTC：纯 SNTP，系统时间即可
  const uint64_t now = static_cast<uint64_t>(time(nullptr));
  if (rtc_write_(now)) {
    ESP_LOGI(TAG, "rtc write-back ok (unix %llu)", static_cast<unsigned long long>(now));
  } else {
    ESP_LOGW(TAG, "rtc write-back failed (system time synced, rtc unchanged)");
  }
}

// SNTP 同步通知落地（TCPIP 上下文 → 原子；net_task service 消费）。
void TimeSync::noteSync(uint64_t unix_sec) {
  synced_.store(true, std::memory_order_relaxed);
  last_sync_at_.store(unix_sec, std::memory_order_relaxed);
  rtc_write_pending_.store(true, std::memory_order_relaxed);
}

}  // namespace eego::net
