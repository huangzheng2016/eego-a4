// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// keep_awake —— 「保持唤醒」（配置模式）覆盖与设备电源状态快照。
//
// 保持唤醒：限时覆盖，期间不进入空闲刷新节奏、不因交互超时深睡、不进入夜间暂停
// 深睡，WiFi 与管理 API 保持在线。状态只在 RAM（单调时钟截止时刻），重启/深睡即
// 失效，到期自动结束。触发来源：设备设置页动作行、状态页软键、可绑定内建动作
// keep_awake（默认 ↑+↓ 同按）、管理 API POST /api/v1/device/keep-awake。
//
// 并发：截止时刻是一个原子量，httpd 与 app_task 都可读写；app_task 通过 revision()
// 发现外部修改（重绘状态行、视为一次交互）。电源快照由 app_task 定期发布，httpd
// 只读（互斥锁保护的小结构）。
//
// 本头的 inline 纯函数无 ESP-IDF 依赖（主机测试直接 include）。

#include <cstdint>
#include <string>

namespace eego::admin {

namespace keep_awake {

inline constexpr uint32_t kDefaultMinutes = 30;
inline constexpr uint32_t kMaxMinutes = 240;  // API 上限（设置项取 10/30/60/120）
inline constexpr uint32_t kChoices[] = {10, 30, 60, 120};

inline bool validSettingMinutes(uint32_t minutes) {
  for (uint32_t choice : kChoices) {
    if (choice == minutes) return true;
  }
  return false;
}

// API 时长：0 = 取消；1..kMaxMinutes = 从现在起保持 N 分钟。
inline bool validRequestMinutes(int64_t minutes) { return minutes >= 0 && minutes <= kMaxMinutes; }

inline int64_t deadlineMs(int64_t now_ms, uint32_t minutes) {
  if (minutes == 0) return 0;
  const int64_t until = now_ms + static_cast<int64_t>(minutes) * 60000;
  return until == 0 ? 1 : until;
}

inline bool active(int64_t now_ms, int64_t until_ms) { return until_ms != 0 && now_ms < until_ms; }

inline uint32_t remainingS(int64_t now_ms, int64_t until_ms) {
  if (!active(now_ms, until_ms)) return 0;
  return static_cast<uint32_t>((until_ms - now_ms + 999) / 1000);
}

// 状态行显示的剩余分钟：向上取整；> 10 分钟时再向上取整到 5 的倍数，使墨水屏
// 至多每 5 分钟（最后 10 分钟每分钟）因倒计时重绘一次。0 = 未开启。
inline uint32_t displayMinutes(uint32_t remaining_s) {
  if (remaining_s == 0) return 0;
  uint32_t minutes = (remaining_s + 59) / 60;
  if (minutes > 10) minutes = (minutes + 4) / 5 * 5;
  return minutes;
}

// 设备端「保持唤醒」切换（设置行/状态页软键/按键绑定）：未开启 → 开启 duration；
// 已开启且剩余不足一半 → 延长回 duration；剩余超过一半 → 取消。返回新的时长
// （分钟；0 = 取消）。
inline uint32_t toggleMinutes(uint32_t remaining_s, uint32_t duration_min) {
  if (duration_min == 0) duration_min = kDefaultMinutes;
  if (remaining_s == 0) return duration_min;
  if (remaining_s <= duration_min * 60 / 2) return duration_min;
  return 0;
}

}  // namespace keep_awake

// ---- 设备侧共享状态（实现见 src/keep_awake.cpp；ESP-IDF 单调时钟 esp_timer）----

// 截止时刻（esp_timer 毫秒；0 = 未开启）。
int64_t keepAwakeUntilMs();
// 从现在起保持 minutes 分钟（0 = 取消）。返回新的截止时刻。
int64_t setKeepAwakeMinutes(uint32_t minutes);
// 每次修改 +1（app_task 据此发现 web 端的修改）。
uint32_t keepAwakeRevision();

// app_task 发布、GET /api/v1/status 读取的电源/休眠状态。
struct PowerSnapshot {
  bool valid = false;
  bool mains = true;
  std::string source;          // "mains" | "battery"
  std::string source_reason;   // battery::powerReasonName
  std::string power_mode;      // 设置：auto | mains | battery
  std::string mode;            // active | keep_awake | idle | awake | sleeping
  int battery_pct = -1;        // -1 = 无读数
  int battery_mv = 0;
  bool charging = false;
  bool usb_host = false;
  uint64_t next_wake = 0;      // 电池模式/夜间暂停：下次定时唤醒（unix；0 = 无）
  uint64_t next_refresh = 0;   // 空闲节奏：下一次屏幕刷新边界（unix；0 = 即时）
  int64_t sleep_in_s = -1;     // 电池交互期：无操作多少秒后深睡（-1 = 不会自动深睡）
  std::string last_wake_reason;
  uint32_t sleeps_since_boot = 0;
  uint64_t last_screen_update = 0;  // 最近一次实际刷屏（unix；0 = 未知）
};
void publishPowerSnapshot(const PowerSnapshot& snapshot);
PowerSnapshot powerSnapshot();

}  // namespace eego::admin
