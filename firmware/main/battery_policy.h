// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// battery_policy —— 电池模式的全部决策（纯逻辑，无 ESP-IDF 依赖，主机可测）。
//
// 电池模式 = 「深睡 → 定时唤醒 → 一批取数 → 必要时刷屏 → 深睡」。能耗排序：
// WiFi 唤醒（关联 + DHCP + TLS，~100 mA 持续数秒）≫ 墨水屏刷新（Full ~2-3 s 高压）
// ≫ 其余。本文件回答三个问题：
//
//   1. 下次何时唤醒（planNextWake）：按本地墙钟对齐到 battery_refresh_min 的整数倍
//      （自本地零点起算，不跨零点漂移）；低电量（<20%）间隔翻倍；连续取数失败按
//      2^n 退避（封顶 kMaxIntervalMin）；夜间暂停窗口内不唤醒（直接睡到窗口结束）；
//      电量 <5% 不设定时器（只有电源键能唤醒）。定时器提前醒（RC 慢时钟漂移）在
//      kEarlyWakeToleranceS 内视为已到达本边界，避免为几十秒多醒一次。
//   2. 这一帧要不要刷（decideRefresh/refreshMode）：帧哈希（1-bit + 灰阶平面）与
//      上次显示的相同且未超过 battery_max_stale_min → 完全不碰屏；否则 Fast（需要
//      已把上一帧预载到控制器 DTM1），每 kFullEvery 次或每天一次 Full，缺少/损坏
//      上一帧时 Full。
//   3. 状态行显示什么：电量（5% 分档 + 滞回，避免 ADC 抖动逼出刷新）、「更新于」
//      = 上次实际刷屏的边界时刻、「最迟」= 最晚何时会再刷一次（由上次刷屏时刻和
//      max-stale 推出）。三者在数据不变时都不变，因此数据不变 ⇒ 帧不变 ⇒ 不刷屏；
//      时钟本身不会逼出刷新。
//
// 跨深睡状态（RtcState）由设备侧放在 RTC_DATA_ATTR 内存里（battery_mode.cpp），
// 本文件只定义布局与校验。

#include <cstddef>
#include <cstdint>
#include <vector>

namespace panel::battery {

inline constexpr uint64_t kMinValidUnix = 946684800ULL;  // 2000-01-01
inline constexpr uint32_t kFullEvery = 12;                // 每 N 次刷屏一次 Full
inline constexpr uint64_t kFullMaxAgeS = 86400;           // 至少每天一次 Full
inline constexpr uint8_t kLowBatteryPct = 20;             // 低于此值间隔翻倍
inline constexpr uint8_t kCriticalBatteryPct = 5;         // 低于此值停止定时唤醒
inline constexpr uint32_t kMaxIntervalMin = 240;          // 退避/翻倍后的间隔上限
inline constexpr uint32_t kMaxBackoffShift = 3;           // 失败退避最多 ×8
inline constexpr uint64_t kEarlyWakeToleranceS = 90;      // 提前醒的容差
inline constexpr uint64_t kMinSleepS = 30;
inline constexpr uint8_t kBatteryBucketPct = 5;           // 状态行电量分档

// ── 时间对齐 ────────────────────────────────────────────────────────────────

// unix 之后（严格大于）的下一个本地对齐边界：自本地零点起每 period_min 分钟一个，
// 且不越过下一个本地零点（period 不整除 1440 时每天重新对齐）。period 0 按 1。
uint64_t alignedBoundaryAfter(uint64_t unix_s, int tz_offset_min, uint32_t period_min);

// unix 所在（或 tolerance 秒内即将到达）的边界时刻：用于把提前几十秒的定时唤醒
// 记为「本边界」（状态行「更新于」显示整点时刻）。
uint64_t nominalSlot(uint64_t unix_s, int tz_offset_min, uint32_t period_min,
                     uint64_t tolerance_s = kEarlyWakeToleranceS);

// 本地 HH:MM（tz 偏移后）。
void localHourMinute(uint64_t unix_s, int tz_offset_min, int& hour, int& minute);

// 每日本地夜间暂停窗口（与 device_settings 的 sleep_start_min/sleep_end_min 相同；
// 支持跨零点；任一为 -1 或二者相等 = 禁用）。
struct NightWindow {
  int start_min = -1;
  int end_min = -1;
  bool enabled() const {
    return start_min >= 0 && end_min >= 0 && start_min < 1440 && end_min < 1440 &&
           start_min != end_min;
  }
};

// unix 是否在窗口内；在则 *end_unix = 本次窗口结束时刻（严格大于 unix）。
bool inNightWindow(uint64_t unix_s, int tz_offset_min, const NightWindow& window,
                   uint64_t* end_unix = nullptr);

// ── 唤醒计划 ────────────────────────────────────────────────────────────────

// 有效间隔：低电量 ×2，连续失败 ×2^min(failures, kMaxBackoffShift)，封顶 kMaxIntervalMin。
uint32_t effectiveIntervalMin(uint32_t base_min, bool battery_known, uint8_t battery_pct,
                              uint32_t consecutive_failures);

enum class WakeKind : uint8_t {
  Boundary = 0,     // 下一个对齐边界
  NightPause,       // 夜间暂停：睡到窗口结束
  CriticalBattery,  // 电量过低：不设定时器
  NoClock,          // 时间不可信：按间隔相对睡眠（不对齐）
};

struct WakeInputs {
  uint64_t now_unix = 0;
  int tz_offset_min = 0;
  uint32_t interval_min = 15;
  NightWindow night;
  bool battery_known = false;
  uint8_t battery_pct = 100;
  bool charging = false;
  uint32_t consecutive_failures = 0;
};

struct WakePlan {
  WakeKind kind = WakeKind::Boundary;
  uint64_t wake_unix = 0;     // 0 = 无定时唤醒（CriticalBattery）或时间不可信
  uint64_t sleep_s = 0;       // 0 = 不设定时器
  uint32_t interval_min = 0;  // 实际使用的有效间隔
};

WakePlan planNextWake(const WakeInputs& in);

// 「最迟」时刻：上次刷屏边界 + max_stale 之后的第一个唤醒边界（夜间窗口内顺延到
// 窗口结束）。只依赖上次刷屏时刻，因此数据不变时状态行文字不变。
uint64_t refreshDeadline(uint64_t last_refresh_unix, uint32_t max_stale_min, int tz_offset_min,
                         uint32_t interval_min, const NightWindow& night);

// ── 刷屏决策 ────────────────────────────────────────────────────────────────

// 上次实际显示到面板上的帧（POD，存于 RTC 内存）。
struct FrameRecord {
  uint64_t hash = 0;
  uint64_t refresh_unix = 0;   // 上次刷屏的名义边界时刻（状态行「更新于」）
  uint64_t full_unix = 0;      // 上次 Full 的时刻
  uint32_t fast_since_full = 0;
  uint8_t valid = 0;           // 0 = 面板内容未知（首次上电/交互期改动过屏幕）
  uint8_t reserved[3] = {0, 0, 0};
};

enum class RefreshAction : uint8_t { Skip = 0, Fast, Full };

// 帧是否已超过 max-stale（时间不可信时视为未超）。max_stale_min = 0 → 恒超。
bool frameStale(const FrameRecord& rec, uint64_t now_unix, uint32_t max_stale_min);

// 刷新所用波形：上一帧未知/未能预载到 DTM1/强制 → Full；第 kFullEvery 次或距上次
// Full ≥ 1 天 → Full；否则 Fast。
RefreshAction refreshMode(const FrameRecord& rec, uint64_t now_unix, bool previous_loaded,
                          bool force_full, uint32_t full_every = kFullEvery);

// 候选帧（以上次的「更新于」渲染）与面板一致且未过期 → Skip；否则 refreshMode。
RefreshAction decideRefresh(const FrameRecord& rec, uint64_t candidate_hash, uint64_t now_unix,
                            uint32_t max_stale_min, bool previous_loaded, bool force_full,
                            uint32_t full_every = kFullEvery);

void noteRefreshed(FrameRecord& rec, RefreshAction action, uint64_t hash, uint64_t slot_unix,
                   uint64_t now_unix);

// 状态行电量分档（向下取整到 5%），带 ±2% 滞回：raw 仍在上次分档 [prev-2, prev+7)
// 内则保持上次分档。prev < 0 = 无上次。
uint8_t batteryDisplayBucket(uint8_t raw_pct, int prev_bucket);

// ── 帧哈希与压缩存储 ────────────────────────────────────────────────────────

uint64_t fnv1a64(const uint8_t* data, size_t size, uint64_t seed = 1469598103934665603ULL);
uint32_t crc32(const uint8_t* data, size_t size, uint32_t seed = 0);

// PackBits：n∈[0,127] → 后随 n+1 字节原样；n∈[-127,-1] → 下一字节重复 1-n 次。
size_t packbitsMaxSize(size_t size);
// 编码到调用方缓冲区（设备侧放 PSRAM）；cap 不足返回 0。
size_t packbitsEncodeTo(const uint8_t* data, size_t size, uint8_t* out, size_t cap);
std::vector<uint8_t> packbitsEncode(const uint8_t* data, size_t size);
// 解出恰好 out_size 字节才返回 true（长度不符/越界 = 损坏）。
bool packbitsDecode(const uint8_t* data, size_t size, uint8_t* out, size_t out_size);

// 帧文件："EBF1" 魔数 + 版本 + 原始长度 + 载荷长度 + 帧哈希 + CRC32 + PackBits 载荷。
inline constexpr uint32_t kFrameMagic = 0x31464245u;  // "EBF1" little-endian
inline constexpr uint16_t kFrameVersion = 1;
inline constexpr size_t kFrameHeaderSize = 28;
size_t frameFileMaxSize(size_t frame_size);
size_t encodeFrameFileTo(const uint8_t* frame, size_t size, uint64_t hash, uint8_t* out,
                         size_t cap);
std::vector<uint8_t> encodeFrameFile(const uint8_t* frame, size_t size, uint64_t hash);
// 校验魔数/版本/长度/CRC 并解压到 out（恰好 size 字节）；hash 返回文件中记录的帧哈希。
bool decodeFrameFile(const uint8_t* file, size_t file_size, uint8_t* out, size_t size,
                     uint64_t& hash);

// ── 跨深睡状态（RTC 内存布局）──────────────────────────────────────────────

struct RtcState {
  uint32_t magic = 0;
  uint32_t version = 0;
  FrameRecord frame;
  uint64_t last_data_unix = 0;       // 上次全部到期源取数成功的时刻
  uint64_t last_wake_unix = 0;
  uint32_t consecutive_failures = 0; // 连续取数失败（WiFi 不通/全部源失败）
  uint32_t wakes = 0;
  int16_t battery_bucket = -1;       // 状态行上次显示的电量分档
  uint8_t ap_valid = 0;              // WiFi 快连缓存（上次关联的 BSSID + 信道）
  uint8_t ap_channel = 0;
  uint8_t ap_bssid[6] = {0, 0, 0, 0, 0, 0};
  char ap_ssid[33] = {0};
  uint8_t low_battery_shown = 0;
  uint32_t checksum = 0;
};

inline constexpr uint32_t kRtcMagic = 0x45424154u;  // "TABE"
inline constexpr uint32_t kRtcVersion = 1;

void rtcReset(RtcState& state);
void rtcSeal(RtcState& state);
bool rtcValid(const RtcState& state);

// 简单供电判定（DeviceSettings PowerMode：0 auto / 1 mains / 2 battery）：auto 时
// 无电池读数或充电中视为常供电。注意：GPIO11 是「充电状态」而非 VBUS——电池充满
// 后充电停止，单凭它会把插着 USB 的设备误判为电池供电。设备侧一律用 MainsDetector。
bool mainsPowered(uint8_t power_mode, bool charging, uint16_t battery_mv);

// ── 供电来源判定（带锁存与滞回）──────────────────────────────────────────────
//
// auto 模式下按优先级：
//   1. 无电池读数（0 mV）                  → 常供电（no_battery）
//   2. 充电中                              → 常供电，并锁存（charging）
//   3. USB 主机已连接（USB-Serial-JTAG SOF）→ 常供电，并锁存（usb_host）
//   4. 已锁存（曾判定为常供电，或冷启动时电压 ≥ kMainsHoldMv）：电池满电后充电停止，
//      只要电压仍 ≥ kMainsHoldMv 且未持续下降就保持常供电（full_hold）；锁存期内
//      记录电压峰值，自峰值起 ≥ kUnplugWindowMs 后电压仍低于峰值 kUnplugDropMv 以上，
//      或电压跌破 kMainsHoldMv → 判定拔电（voltage_drop / below_hold）；
//   5. 其余                                → 电池（battery）。
// 强制模式：mains → 常供电（forced_mains）；battery → 电池（forced_battery）。
inline constexpr uint16_t kMainsHoldMv = 4050;
inline constexpr uint16_t kUnplugDropMv = 60;
inline constexpr int64_t kUnplugWindowMs = 120000;

enum class PowerReason : uint8_t {
  ForcedMains = 0,
  ForcedBattery,
  NoBattery,
  Charging,
  UsbHost,
  FullHold,
  VoltageDrop,
  BelowHold,
  Battery,
};
const char* powerReasonName(PowerReason reason);  // 稳定的 API/日志名（snake_case）

struct PowerSample {
  bool charging = false;
  bool usb_host = false;
  uint16_t battery_mv = 0;
};

class MainsDetector {
 public:
  // 冷启动（非深睡唤醒）且电压 ≥ kMainsHoldMv：插着充电器开机、电池已满时充电
  // 指示不亮，预先锁存，随后由滞回在几分钟内识别是否真的在用电池。
  void seedColdBoot(uint16_t battery_mv, int64_t now_ms);
  // 每次新采样（或模式变化）调用；返回是否常供电。
  bool update(uint8_t power_mode, const PowerSample& sample, int64_t now_ms);
  bool mains() const { return mains_; }
  PowerReason reason() const { return reason_; }
  bool latched() const { return latched_; }

 private:
  void latch(uint16_t mv, int64_t now_ms);
  bool mains_ = true;
  PowerReason reason_ = PowerReason::NoBattery;
  bool latched_ = false;
  uint16_t peak_mv_ = 0;
  int64_t peak_ms_ = 0;
};

// 电池交互期 / 常供电夜间暂停是否应进入深睡：保持唤醒（keep_awake_until_ms 为单调
// 时钟截止时刻，0 = 未开启）期间一律不睡；到期后按最后一次活动重新计时（不额外
// 宽限：过期即恢复原节奏）。
bool keepAwakeBlocks(int64_t now_ms, int64_t keep_awake_until_ms);
bool interactiveSleepDue(int64_t now_ms, int64_t last_activity_ms, uint32_t timeout_s,
                         int64_t keep_awake_until_ms);

}  // namespace panel::battery
