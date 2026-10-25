// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// battery_mode —— 电池模式的设备侧胶水（ESP-IDF）：
//   * 跨深睡状态：RtcState 放在 RTC_DATA_ATTR 内存（深睡保持、上电清零；校验和不符
//     即重置），记录上次显示帧哈希/刷屏时刻/Full 计数、电量分档、连续失败、WiFi 快连
//     缓存（BSSID + 信道，不含密码）；
//   * 上一帧存储：面板实际显示的 B/W 帧经 PackBits 压缩后以 FatFS 安全的原子替换
//     写到 SD（/sdcard/alertpanel/battery_frame.bin），唤醒后读回预载到控制器 DTM1，
//     使深睡后也能 Fast 刷新；
//   * 唤醒源：定时器（对齐边界）+ 电源键 EXT0（GPIO8 高有效）+ ↑/↓ EXT1
//     （GPIO5/GPIO7 均为 RTC GPIO、低有效 → ESP_EXT1_WAKEUP_ANY_LOW，深睡期间
//     保持 RTC 上拉）。触控 GSLX680 无 INT 引脚（I²C 轮询），不能唤醒。
// 决策逻辑全部在 battery_policy（纯逻辑，主机测试）。

#include <cstddef>
#include <cstdint>

#include "battery_policy.h"

namespace panel::battery {

enum class WakeSource : uint8_t {
  PowerOn = 0,  // 上电/复位/软件重启（非深睡唤醒）
  Timer,        // 对齐边界定时唤醒 → 快速路径
  PowerKey,     // EXT0
  Buttons,      // EXT1（↑/↓）
  Other,
};

WakeSource wakeSource();
const char* wakeSourceName(WakeSource source);
inline bool interactiveWake(WakeSource s) {
  return s == WakeSource::PowerKey || s == WakeSource::Buttons;
}
inline bool deepSleepWake(WakeSource s) { return s != WakeSource::PowerOn; }

// RTC 内存中的跨深睡状态（首次访问时校验，无效则重置）。修改后调用 commitRtcState()。
RtcState& rtcState();
void commitRtcState();

// 上一帧文件（SD）。load 只在文件完整（CRC）且记录的哈希 == expected_hash 时成功。
inline constexpr const char* kFramePath = "/sdcard/alertpanel/battery_frame.bin";
bool loadStoredFrame(uint8_t* out, size_t size, uint64_t expected_hash);
bool storeFrame(const uint8_t* frame, size_t size, uint64_t hash);

// 配置唤醒源并进入深睡（不返回）。sleep_s = 0 → 不设定时器；buttons → ↑/↓ EXT1。
// 调用前须已关闭 WiFi/BLE/前光、墨水屏控制器已深睡。
[[noreturn]] void deepSleep(uint64_t sleep_s, bool buttons);

}  // namespace panel::battery
