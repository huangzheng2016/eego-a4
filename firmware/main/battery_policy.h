// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

//
//
//

#include <cstddef>
#include <cstdint>
#include <vector>

namespace panel::battery {
inline constexpr uint64_t kMinValidUnix = 946684800ULL;  // 2000-01-01
inline constexpr uint32_t kFullEvery = 12;
inline constexpr uint64_t kFullMaxAgeS = 86400;
inline constexpr uint8_t kLowBatteryPct = 20;
inline constexpr uint8_t kCriticalBatteryPct = 5;
// Critical sleep still wakes on this timer: a radio-free voltage check (~0.2 s) that
// resumes normal operation once charging or back above kCriticalExitPct.
inline constexpr uint64_t kCriticalRecheckS = 3600;
inline constexpr uint8_t kCriticalExitPct = 8;
// A battery last read at or above this cannot be critical one wake later: such a
// reading is a glitch (radio load, ADC noise) until a second wake confirms it.
inline constexpr uint16_t kPlausibleCriticalFromMv = 3600;
inline constexpr uint32_t kMaxIntervalMin = 240;
inline constexpr uint32_t kMaxBackoffShift = 3;
inline constexpr uint64_t kEarlyWakeToleranceS = 90;
inline constexpr uint64_t kMinSleepS = 30;
inline constexpr uint8_t kBatteryBucketPct = 5;

uint64_t alignedBoundaryAfter(uint64_t unix_s, int tz_offset_min, uint32_t period_min);

uint64_t nominalSlot(uint64_t unix_s, int tz_offset_min, uint32_t period_min,
                     uint64_t tolerance_s = kEarlyWakeToleranceS);

void localHourMinute(uint64_t unix_s, int tz_offset_min, int& hour, int& minute);

struct NightWindow {
  int start_min = -1;
  int end_min = -1;
  bool enabled() const {
    return start_min >= 0 && end_min >= 0 && start_min < 1440 && end_min < 1440 &&
           start_min != end_min;
  }
};

bool inNightWindow(uint64_t unix_s, int tz_offset_min, const NightWindow& window,
                   uint64_t* end_unix = nullptr);

uint32_t effectiveIntervalMin(uint32_t base_min, bool battery_known, uint8_t battery_pct,
                              uint32_t consecutive_failures);

enum class WakeKind : uint8_t {
  Boundary = 0,
  NightPause,
  CriticalBattery,
  NoClock,
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
  uint64_t wake_unix = 0;
  uint64_t sleep_s = 0;
  uint32_t interval_min = 0;
};

WakePlan planNextWake(const WakeInputs& in);

uint64_t refreshDeadline(uint64_t last_refresh_unix, uint32_t max_stale_min, int tz_offset_min,
                         uint32_t interval_min, const NightWindow& night);

struct FrameRecord {
  uint64_t hash = 0;
  uint64_t refresh_unix = 0;
  uint64_t full_unix = 0;
  uint32_t fast_since_full = 0;
  uint8_t valid = 0;
  uint8_t reserved[3] = {0, 0, 0};
};

enum class RefreshAction : uint8_t { Skip = 0, Fast, Full };

bool frameStale(const FrameRecord& rec, uint64_t now_unix, uint32_t max_stale_min);

RefreshAction refreshMode(const FrameRecord& rec, uint64_t now_unix, bool previous_loaded,
                          bool force_full, uint32_t full_every = kFullEvery);

RefreshAction decideRefresh(const FrameRecord& rec, uint64_t candidate_hash, uint64_t now_unix,
                            uint32_t max_stale_min, bool previous_loaded, bool force_full,
                            uint32_t full_every = kFullEvery);

void noteRefreshed(FrameRecord& rec, RefreshAction action, uint64_t hash, uint64_t slot_unix,
                   uint64_t now_unix);

uint8_t batteryDisplayBucket(uint8_t raw_pct, int prev_bucket);

uint64_t fnv1a64(const uint8_t* data, size_t size, uint64_t seed = 1469598103934665603ULL);
uint32_t crc32(const uint8_t* data, size_t size, uint32_t seed = 0);

size_t packbitsMaxSize(size_t size);
size_t packbitsEncodeTo(const uint8_t* data, size_t size, uint8_t* out, size_t cap);
std::vector<uint8_t> packbitsEncode(const uint8_t* data, size_t size);
bool packbitsDecode(const uint8_t* data, size_t size, uint8_t* out, size_t out_size);

inline constexpr uint32_t kFrameMagic = 0x31464245u;  // "EBF1" little-endian
inline constexpr uint16_t kFrameVersion = 1;
inline constexpr size_t kFrameHeaderSize = 28;
size_t frameFileMaxSize(size_t frame_size);
size_t encodeFrameFileTo(const uint8_t* frame, size_t size, uint64_t hash, uint8_t* out,
                         size_t cap);
std::vector<uint8_t> encodeFrameFile(const uint8_t* frame, size_t size, uint64_t hash);
bool decodeFrameFile(const uint8_t* file, size_t file_size, uint8_t* out, size_t size,
                     uint64_t& hash);

struct RtcState {
  uint32_t magic = 0;
  uint32_t version = 0;
  FrameRecord frame;
  uint64_t last_data_unix = 0;
  uint64_t last_wake_unix = 0;
  uint32_t consecutive_failures = 0;
  uint32_t wakes = 0;
  int16_t battery_bucket = -1;
  uint8_t ap_valid = 0;
  uint8_t ap_channel = 0;
  uint8_t ap_bssid[6] = {0, 0, 0, 0, 0, 0};
  char ap_ssid[33] = {0};
  uint8_t low_battery_shown = 0;
  uint8_t manual_sleep = 0;
  uint8_t charge_suspect = 0;
  uint8_t critical_strikes = 0;
  uint16_t last_battery_mv = 0;
  uint32_t checksum = 0;
};

inline constexpr uint32_t kRtcMagic = 0x45424154u;  // "TABE"
inline constexpr uint32_t kRtcVersion = 4;

void rtcReset(RtcState& state);
void rtcSeal(RtcState& state);
bool rtcValid(const RtcState& state);

bool mainsPowered(uint8_t power_mode, bool charging, uint16_t battery_mv);

// A critical reading is acted on only when it is plausible: the previous accepted
// reading was already low (or unknown), or an earlier wake already saw it (strikes).
bool acceptCriticalReading(uint16_t last_battery_mv, uint8_t strikes);

//
//
inline constexpr uint16_t kHoldEnterMv = 4150;
inline constexpr uint16_t kHoldExitMv = 4120;
inline constexpr uint16_t kUnplugDropMv = 25;
inline constexpr int64_t kDropWindowMs = 180000;
inline constexpr int64_t kSettleMs = 120000;
inline constexpr int64_t kChargeRecentMs = 20 * 60000;
inline constexpr int64_t kChargeOffDebounceMs = 10000;
inline constexpr int64_t kFastBatterySampleMs = 10000;
// The charge pin says "charging" but the filtered battery voltage sits this far below
// its peak since charging began, for kChargeSuspectSamples fresh readings in a row:
// the pin polarity is wrong (or the charger cannot keep up), so run on battery.
inline constexpr uint16_t kChargeSuspectDropMv = 40;
inline constexpr uint32_t kChargeSuspectSamples = 3;

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
  ChargeSuspect,
};
const char* powerReasonName(PowerReason reason);

struct PowerSample {
  bool charging = false;
  bool usb_host = false;
  uint16_t battery_mv = 0;
  int64_t battery_read_ms = -1;
};

class MainsDetector {
 public:
  bool update(uint8_t power_mode, const PowerSample& sample, int64_t now_ms);
  bool mains() const { return mains_; }
  PowerReason reason() const { return reason_; }
  bool latched() const { return latched_; }
  bool wantsFastSamples() const { return armed_; }
  // The charge pin is not trusted: it reports charging while the battery drains.
  bool chargeSuspect() const { return charge_suspect_; }
  // Restores the flag after deep sleep (the voltage history is lost on wake).
  void seedChargeSuspect(bool suspect) { charge_suspect_ = suspect; }

 private:
  struct Point {
    int64_t ms;
    uint16_t mv;
  };
  static constexpr size_t kHistoryCap = 48;
  void decide(bool mains, PowerReason reason);
  uint16_t filteredMv() const;
  bool dropDetected(uint16_t filtered) const;
  void pushHistory(int64_t ms, uint16_t mv);

  bool mains_ = false;
  PowerReason reason_ = PowerReason::Battery;
  bool charging_ = false;
  int64_t charge_off_since_ms_ = -1;
  int64_t last_charge_ms_ = 0;
  bool usb_host_prev_ = false;
  bool armed_ = false;
  bool latched_ = false;
  int64_t settle_until_ms_ = 0;
  int64_t last_read_ms_ = -1;
  bool charge_suspect_ = false;
  uint16_t charge_peak_mv_ = 0;
  uint32_t charge_low_count_ = 0;
  uint16_t raw_[3] = {0, 0, 0};
  size_t raw_count_ = 0;
  Point history_[kHistoryCap] = {};
  size_t history_count_ = 0;
};

bool keepAwakeBlocks(int64_t now_ms, int64_t keep_awake_until_ms);
bool interactiveSleepDue(int64_t now_ms, int64_t last_activity_ms, uint32_t timeout_s,
                         int64_t keep_awake_until_ms);

}  // namespace panel::battery
