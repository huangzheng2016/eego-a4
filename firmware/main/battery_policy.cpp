// SPDX-License-Identifier: GPL-3.0-or-later
// 电池模式决策（契约见 battery_policy.h）。
#include "battery_policy.h"

#include <algorithm>
#include <cstring>

namespace panel::battery {

namespace {

constexpr int64_t kDayS = 86400;

int64_t localOf(uint64_t unix_s, int tz_offset_min) {
  return static_cast<int64_t>(unix_s) + static_cast<int64_t>(tz_offset_min) * 60;
}

int64_t floorDiv(int64_t a, int64_t b) {
  int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
  return q;
}

int64_t floorMod(int64_t a, int64_t b) { return a - floorDiv(a, b) * b; }

void putU16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v));
  out.push_back(static_cast<uint8_t>(v >> 8));
}

void putU32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void putU64(std::vector<uint8_t>& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

uint64_t getLe(const uint8_t* p, int bytes) {
  uint64_t v = 0;
  for (int i = bytes - 1; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

template <typename T>
uint64_t mix(uint64_t h, T value) {
  uint8_t bytes[sizeof(T)];
  std::memcpy(bytes, &value, sizeof(T));
  return fnv1a64(bytes, sizeof(T), h);
}

}  // namespace

// ── 时间对齐 ────────────────────────────────────────────────────────────────

uint64_t alignedBoundaryAfter(uint64_t unix_s, int tz_offset_min, uint32_t period_min) {
  const int64_t period = static_cast<int64_t>(period_min == 0 ? 1 : period_min) * 60;
  const int64_t local = localOf(unix_s, tz_offset_min);
  const int64_t day_start = floorDiv(local, kDayS) * kDayS;
  const int64_t into_day = local - day_start;
  int64_t next = day_start + (into_day / period + 1) * period;
  if (next > day_start + kDayS) next = day_start + kDayS;  // 每天零点重新对齐
  return static_cast<uint64_t>(next - static_cast<int64_t>(tz_offset_min) * 60);
}

uint64_t nominalSlot(uint64_t unix_s, int tz_offset_min, uint32_t period_min,
                     uint64_t tolerance_s) {
  const int64_t period = static_cast<int64_t>(period_min == 0 ? 1 : period_min) * 60;
  const int64_t local = localOf(unix_s + tolerance_s, tz_offset_min);
  const int64_t day_start = floorDiv(local, kDayS) * kDayS;
  const int64_t slot = day_start + ((local - day_start) / period) * period;
  return static_cast<uint64_t>(slot - static_cast<int64_t>(tz_offset_min) * 60);
}

void localHourMinute(uint64_t unix_s, int tz_offset_min, int& hour, int& minute) {
  const int64_t minute_of_day = floorMod(localOf(unix_s, tz_offset_min), kDayS) / 60;
  hour = static_cast<int>(minute_of_day / 60);
  minute = static_cast<int>(minute_of_day % 60);
}

bool inNightWindow(uint64_t unix_s, int tz_offset_min, const NightWindow& window,
                   uint64_t* end_unix) {
  if (!window.enabled() || unix_s < kMinValidUnix) return false;
  const int64_t local = localOf(unix_s, tz_offset_min);
  const int64_t day_start = floorDiv(local, kDayS) * kDayS;
  const int64_t sec_of_day = local - day_start;
  const int64_t start = static_cast<int64_t>(window.start_min) * 60;
  const int64_t end = static_cast<int64_t>(window.end_min) * 60;
  int64_t end_local = 0;
  if (start < end) {
    if (sec_of_day < start || sec_of_day >= end) return false;
    end_local = day_start + end;
  } else {
    if (sec_of_day >= start) {
      end_local = day_start + kDayS + end;
    } else if (sec_of_day < end) {
      end_local = day_start + end;
    } else {
      return false;
    }
  }
  if (end_unix != nullptr) {
    *end_unix = static_cast<uint64_t>(end_local - static_cast<int64_t>(tz_offset_min) * 60);
  }
  return true;
}

// ── 唤醒计划 ────────────────────────────────────────────────────────────────

uint32_t effectiveIntervalMin(uint32_t base_min, bool battery_known, uint8_t battery_pct,
                              uint32_t consecutive_failures) {
  uint64_t interval = base_min == 0 ? 1 : base_min;
  if (battery_known && battery_pct < kLowBatteryPct) interval *= 2;
  interval <<= std::min(consecutive_failures, kMaxBackoffShift);
  return static_cast<uint32_t>(std::min<uint64_t>(interval, kMaxIntervalMin));
}

WakePlan planNextWake(const WakeInputs& in) {
  WakePlan plan;
  if (in.battery_known && !in.charging && in.battery_pct < kCriticalBatteryPct) {
    plan.kind = WakeKind::CriticalBattery;
    return plan;
  }
  plan.interval_min = effectiveIntervalMin(in.interval_min, in.battery_known, in.battery_pct,
                                           in.consecutive_failures);
  if (in.now_unix < kMinValidUnix) {
    plan.kind = WakeKind::NoClock;
    plan.sleep_s = static_cast<uint64_t>(plan.interval_min) * 60;
    return plan;
  }
  uint64_t end = 0;
  if (inNightWindow(in.now_unix, in.tz_offset_min, in.night, &end)) {
    plan.kind = WakeKind::NightPause;
    plan.wake_unix = end;
    plan.sleep_s = std::max<uint64_t>(end - in.now_unix, kMinSleepS);
    return plan;
  }
  const uint64_t candidate =
      alignedBoundaryAfter(in.now_unix + kEarlyWakeToleranceS, in.tz_offset_min, plan.interval_min);
  if (inNightWindow(candidate, in.tz_offset_min, in.night, &end)) {
    plan.kind = WakeKind::NightPause;
    plan.wake_unix = end;
  } else {
    plan.kind = WakeKind::Boundary;
    plan.wake_unix = candidate;
  }
  plan.sleep_s = std::max<uint64_t>(plan.wake_unix - in.now_unix, kMinSleepS);
  return plan;
}

uint64_t refreshDeadline(uint64_t last_refresh_unix, uint32_t max_stale_min, int tz_offset_min,
                         uint32_t interval_min, const NightWindow& night) {
  if (last_refresh_unix < kMinValidUnix) return 0;
  const uint64_t due = last_refresh_unix + static_cast<uint64_t>(max_stale_min) * 60;
  // 第一个 ≥ due 的唤醒边界（due 恰在边界上即该边界）。
  uint64_t wake = alignedBoundaryAfter(due - 1, tz_offset_min, interval_min);
  uint64_t end = 0;
  if (inNightWindow(wake, tz_offset_min, night, &end)) wake = end;
  return wake;
}

// ── 刷屏决策 ────────────────────────────────────────────────────────────────

bool frameStale(const FrameRecord& rec, uint64_t now_unix, uint32_t max_stale_min) {
  if (max_stale_min == 0) return true;
  if (now_unix < kMinValidUnix || rec.refresh_unix < kMinValidUnix) return false;
  if (now_unix < rec.refresh_unix) return false;  // 墙钟回拨：不据此强刷
  // 容差同唤醒：提前几十秒醒来时，本应到期的强刷仍在本次完成。
  return now_unix + kEarlyWakeToleranceS - rec.refresh_unix >=
         static_cast<uint64_t>(max_stale_min) * 60;
}

RefreshAction refreshMode(const FrameRecord& rec, uint64_t now_unix, bool previous_loaded,
                          bool force_full, uint32_t full_every) {
  if (rec.valid == 0 || !previous_loaded || force_full) return RefreshAction::Full;
  if (full_every != 0 && rec.fast_since_full + 1 >= full_every) return RefreshAction::Full;
  if (now_unix >= kMinValidUnix && rec.full_unix >= kMinValidUnix && now_unix >= rec.full_unix &&
      now_unix - rec.full_unix >= kFullMaxAgeS) {
    return RefreshAction::Full;
  }
  if (rec.full_unix < kMinValidUnix && now_unix >= kMinValidUnix) return RefreshAction::Full;
  return RefreshAction::Fast;
}

RefreshAction decideRefresh(const FrameRecord& rec, uint64_t candidate_hash, uint64_t now_unix,
                            uint32_t max_stale_min, bool previous_loaded, bool force_full,
                            uint32_t full_every) {
  if (rec.valid != 0 && !force_full && candidate_hash == rec.hash &&
      !frameStale(rec, now_unix, max_stale_min)) {
    return RefreshAction::Skip;
  }
  return refreshMode(rec, now_unix, previous_loaded, force_full, full_every);
}

void noteRefreshed(FrameRecord& rec, RefreshAction action, uint64_t hash, uint64_t slot_unix,
                   uint64_t now_unix) {
  if (action == RefreshAction::Skip) return;
  rec.hash = hash;
  rec.refresh_unix = slot_unix;
  rec.valid = 1;
  if (action == RefreshAction::Full) {
    rec.fast_since_full = 0;
    rec.full_unix = now_unix >= kMinValidUnix ? now_unix : 0;
  } else {
    ++rec.fast_since_full;
  }
}

uint8_t batteryDisplayBucket(uint8_t raw_pct, int prev_bucket) {
  const int raw = std::min<int>(raw_pct, 100);
  if (prev_bucket >= 0 && prev_bucket <= 100 && raw >= prev_bucket - 2 &&
      raw < prev_bucket + kBatteryBucketPct + 2) {
    return static_cast<uint8_t>(prev_bucket);
  }
  return static_cast<uint8_t>((raw / kBatteryBucketPct) * kBatteryBucketPct);
}

// ── 帧哈希与压缩存储 ────────────────────────────────────────────────────────

uint64_t fnv1a64(const uint8_t* data, size_t size, uint64_t seed) {
  uint64_t h = seed;
  for (size_t i = 0; i < size; ++i) {
    h ^= data[i];
    h *= 1099511628211ULL;
  }
  return h;
}

uint32_t crc32(const uint8_t* data, size_t size, uint32_t seed) {
  uint32_t crc = ~seed;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

size_t packbitsMaxSize(size_t size) { return size + (size + 127) / 128; }

size_t packbitsEncodeTo(const uint8_t* data, size_t size, uint8_t* out, size_t cap) {
  size_t o = 0;
  size_t i = 0;
  while (i < size) {
    size_t run = 1;
    while (i + run < size && run < 128 && data[i + run] == data[i]) ++run;
    if (run >= 2) {
      if (o + 2 > cap) return 0;
      out[o++] = static_cast<uint8_t>(static_cast<int8_t>(1 - static_cast<int>(run)));
      out[o++] = data[i];
      i += run;
      continue;
    }
    // 字面段：直到出现 ≥2 的重复或满 128 字节（首字节必不与下一字节相同）。
    const size_t start = i;
    size_t len = 0;
    do {
      ++i;
      ++len;
    } while (i < size && len < 128 && !(i + 1 < size && data[i + 1] == data[i]));
    if (o + 1 + len > cap) return 0;
    out[o++] = static_cast<uint8_t>(len - 1);
    std::memcpy(out + o, data + start, len);
    o += len;
  }
  return o;
}

std::vector<uint8_t> packbitsEncode(const uint8_t* data, size_t size) {
  std::vector<uint8_t> out(packbitsMaxSize(size));
  out.resize(packbitsEncodeTo(data, size, out.data(), out.size()));
  return out;
}

bool packbitsDecode(const uint8_t* data, size_t size, uint8_t* out, size_t out_size) {
  size_t i = 0;
  size_t o = 0;
  while (i < size) {
    const int8_t n = static_cast<int8_t>(data[i++]);
    if (n >= 0) {
      const size_t len = static_cast<size_t>(n) + 1;
      if (i + len > size || o + len > out_size) return false;
      std::memcpy(out + o, data + i, len);
      i += len;
      o += len;
    } else if (n != -128) {
      const size_t len = static_cast<size_t>(1 - n);
      if (i >= size || o + len > out_size) return false;
      std::memset(out + o, data[i++], len);
      o += len;
    }
  }
  return o == out_size;
}

size_t frameFileMaxSize(size_t frame_size) {
  return kFrameHeaderSize + packbitsMaxSize(frame_size);
}

size_t encodeFrameFileTo(const uint8_t* frame, size_t size, uint64_t hash, uint8_t* out,
                         size_t cap) {
  if (cap < kFrameHeaderSize) return 0;
  const size_t payload_size =
      packbitsEncodeTo(frame, size, out + kFrameHeaderSize, cap - kFrameHeaderSize);
  if (payload_size == 0 && size != 0) return 0;
  std::vector<uint8_t> header;
  header.reserve(kFrameHeaderSize);
  putU32(header, kFrameMagic);
  putU16(header, kFrameVersion);
  putU16(header, 0);  // flags（保留）
  putU32(header, static_cast<uint32_t>(size));
  putU32(header, static_cast<uint32_t>(payload_size));
  putU64(header, hash);
  putU32(header, crc32(out + kFrameHeaderSize, payload_size));
  std::memcpy(out, header.data(), kFrameHeaderSize);
  return kFrameHeaderSize + payload_size;
}

std::vector<uint8_t> encodeFrameFile(const uint8_t* frame, size_t size, uint64_t hash) {
  std::vector<uint8_t> out(frameFileMaxSize(size));
  out.resize(encodeFrameFileTo(frame, size, hash, out.data(), out.size()));
  return out;
}

bool decodeFrameFile(const uint8_t* file, size_t file_size, uint8_t* out, size_t size,
                     uint64_t& hash) {
  if (file == nullptr || file_size < kFrameHeaderSize) return false;
  if (getLe(file, 4) != kFrameMagic || getLe(file + 4, 2) != kFrameVersion) return false;
  const uint64_t raw_size = getLe(file + 8, 4);
  const uint64_t payload_size = getLe(file + 12, 4);
  if (raw_size != size || payload_size != file_size - kFrameHeaderSize) return false;
  const uint8_t* payload = file + kFrameHeaderSize;
  if (crc32(payload, payload_size) != static_cast<uint32_t>(getLe(file + 24, 4))) return false;
  if (!packbitsDecode(payload, payload_size, out, size)) return false;
  hash = getLe(file + 16, 8);
  return true;
}

// ── 跨深睡状态 ──────────────────────────────────────────────────────────────

namespace {

uint32_t rtcChecksum(const RtcState& s) {
  uint64_t h = 1469598103934665603ULL;
  h = mix(h, s.magic);
  h = mix(h, s.version);
  h = mix(h, s.frame.hash);
  h = mix(h, s.frame.refresh_unix);
  h = mix(h, s.frame.full_unix);
  h = mix(h, s.frame.fast_since_full);
  h = mix(h, s.frame.valid);
  h = mix(h, s.last_data_unix);
  h = mix(h, s.last_wake_unix);
  h = mix(h, s.consecutive_failures);
  h = mix(h, s.wakes);
  h = mix(h, s.battery_bucket);
  h = mix(h, s.ap_valid);
  h = mix(h, s.ap_channel);
  h = fnv1a64(s.ap_bssid, sizeof(s.ap_bssid), h);
  h = fnv1a64(reinterpret_cast<const uint8_t*>(s.ap_ssid), sizeof(s.ap_ssid), h);
  h = mix(h, s.low_battery_shown);
  return static_cast<uint32_t>(h ^ (h >> 32));
}

}  // namespace

void rtcReset(RtcState& state) {
  state = RtcState{};
  state.magic = kRtcMagic;
  state.version = kRtcVersion;
  rtcSeal(state);
}

void rtcSeal(RtcState& state) {
  state.ap_ssid[sizeof(state.ap_ssid) - 1] = '\0';
  state.checksum = rtcChecksum(state);
}

bool rtcValid(const RtcState& state) {
  return state.magic == kRtcMagic && state.version == kRtcVersion &&
         state.checksum == rtcChecksum(state);
}

bool mainsPowered(uint8_t power_mode, bool charging, uint16_t battery_mv) {
  if (power_mode == 1) return true;
  if (power_mode == 2) return false;
  if (battery_mv == 0) return true;  // 无电池读数（台式/未装电池）
  return charging;
}

const char* powerReasonName(PowerReason reason) {
  switch (reason) {
    case PowerReason::ForcedMains: return "forced_mains";
    case PowerReason::ForcedBattery: return "forced_battery";
    case PowerReason::NoBattery: return "no_battery";
    case PowerReason::Charging: return "charging";
    case PowerReason::UsbHost: return "usb_host";
    case PowerReason::FullHold: return "full_hold";
    case PowerReason::VoltageDrop: return "voltage_drop";
    case PowerReason::BelowHold: return "below_hold";
    case PowerReason::Battery: return "battery";
  }
  return "battery";
}

void MainsDetector::latch(uint16_t mv, int64_t now_ms) {
  latched_ = true;
  peak_mv_ = mv;
  peak_ms_ = now_ms;
}

void MainsDetector::seedColdBoot(uint16_t battery_mv, int64_t now_ms) {
  if (battery_mv >= kMainsHoldMv) latch(battery_mv, now_ms);
}

bool MainsDetector::update(uint8_t power_mode, const PowerSample& s, int64_t now_ms) {
  if (power_mode == 1) {
    mains_ = true;
    reason_ = PowerReason::ForcedMains;
    return mains_;
  }
  if (power_mode == 2) {
    mains_ = false;
    reason_ = PowerReason::ForcedBattery;
    latched_ = false;
    return mains_;
  }
  if (s.battery_mv == 0) {
    mains_ = true;
    reason_ = PowerReason::NoBattery;
    return mains_;
  }
  if (s.charging || s.usb_host) {
    // 外部供电确凿：锁存并以当前电压为峰值基准（充电中电压被抬高，停充后回落
    // 到满电静置电压不算拔电——峰值在不充电时才开始跟踪）。
    latch(s.battery_mv, now_ms);
    mains_ = true;
    reason_ = s.charging ? PowerReason::Charging : PowerReason::UsbHost;
    return mains_;
  }
  if (latched_) {
    if (reason_ == PowerReason::Charging || reason_ == PowerReason::UsbHost) {
      // 刚停止充电/USB 主机断开：从此刻重新建立峰值基准。
      peak_mv_ = s.battery_mv;
      peak_ms_ = now_ms;
    }
    if (s.battery_mv < kMainsHoldMv) {
      latched_ = false;
      mains_ = false;
      reason_ = PowerReason::BelowHold;
      return mains_;
    }
    if (s.battery_mv >= peak_mv_) {
      peak_mv_ = s.battery_mv;
      peak_ms_ = now_ms;
    } else if (peak_mv_ - s.battery_mv >= kUnplugDropMv && now_ms - peak_ms_ >= kUnplugWindowMs) {
      latched_ = false;
      mains_ = false;
      reason_ = PowerReason::VoltageDrop;
      return mains_;
    }
    mains_ = true;
    reason_ = PowerReason::FullHold;
    return mains_;
  }
  mains_ = false;
  if (reason_ != PowerReason::VoltageDrop && reason_ != PowerReason::BelowHold) {
    reason_ = PowerReason::Battery;
  }
  return mains_;
}

bool keepAwakeBlocks(int64_t now_ms, int64_t keep_awake_until_ms) {
  return keep_awake_until_ms != 0 && now_ms < keep_awake_until_ms;
}

bool interactiveSleepDue(int64_t now_ms, int64_t last_activity_ms, uint32_t timeout_s,
                         int64_t keep_awake_until_ms) {
  if (keepAwakeBlocks(now_ms, keep_awake_until_ms)) return false;
  return now_ms - last_activity_ms >= static_cast<int64_t>(timeout_s) * 1000;
}

}  // namespace panel::battery
