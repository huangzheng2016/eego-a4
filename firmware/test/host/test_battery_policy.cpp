// SPDX-License-Identifier: GPL-3.0-or-later
// main/battery_policy: aligned battery wakes (timezone, night pause, low battery,
// failure backoff), refresh decisions (hash, max-stale, full-every-N, daily full),
// status-line inputs (battery bucket hysteresis, deadline), frame codec and RTC state.
#include <cstring>
#include <vector>

#include "battery_policy.h"
#include "test_framework.h"

using namespace panel::battery;

namespace {

// 2026-10-25 02:00:00 UTC (= 10:00:00 at UTC+8).
constexpr uint64_t kBase = 1792893600ULL;
constexpr uint64_t kMinS = 60;
constexpr uint64_t kHourS = 3600;
constexpr int kTz = 480;
// Local midnight (UTC+8) of the kBase day.
constexpr uint64_t kMidnight = kBase - 10 * kHourS;

WakeInputs inputs(uint64_t now, uint32_t interval = 15) {
  WakeInputs in;
  in.now_unix = now;
  in.tz_offset_min = kTz;
  in.interval_min = interval;
  in.battery_known = true;
  in.battery_pct = 80;
  return in;
}

FrameRecord record(uint64_t hash, uint64_t refreshed, uint32_t fast_since_full = 0,
                   uint64_t full = kBase) {
  FrameRecord r;
  r.hash = hash;
  r.refresh_unix = refreshed;
  r.full_unix = full;
  r.fast_since_full = fast_since_full;
  r.valid = 1;
  return r;
}

}  // namespace

TEST(battery_policy, aligned_boundaries_follow_local_wall_clock) {
  CHECK_EQ(alignedBoundaryAfter(kBase, kTz, 15), kBase + 15 * kMinS);
  CHECK_EQ(alignedBoundaryAfter(kBase + 1, kTz, 15), kBase + 15 * kMinS);
  CHECK_EQ(alignedBoundaryAfter(kBase + 14 * kMinS + 59, kTz, 15), kBase + 15 * kMinS);
  CHECK_EQ(alignedBoundaryAfter(kBase + 7 * kMinS, kTz, 60), kBase + kHourS);
  // UTC+5:45: local 07:45 at kBase → next quarter hour is 08:00 local = kBase+15min.
  CHECK_EQ(alignedBoundaryAfter(kBase + 1, 345, 15), kBase + 15 * kMinS);
  // Hourly at UTC+5:45 lands on :15 UTC.
  CHECK_EQ(alignedBoundaryAfter(kBase + 1, 345, 60), kBase + 15 * kMinS);
  // Periods that do not divide a day re-align at local midnight (no daily drift).
  const uint64_t late = kMidnight + 23 * kHourS + 30 * kMinS;  // 23:30 local
  CHECK_EQ(alignedBoundaryAfter(late, kTz, 160), kMidnight + 24 * kHourS);
  CHECK_EQ(alignedBoundaryAfter(kMidnight + 24 * kHourS, kTz, 160),
           kMidnight + 24 * kHourS + 160 * kMinS);
}

TEST(battery_policy, nominal_slot_absorbs_early_timer_wake) {
  // Woke 40 s before 10:15 → recorded as the 10:15 slot.
  CHECK_EQ(nominalSlot(kBase + 15 * kMinS - 40, kTz, 15), kBase + 15 * kMinS);
  // Woke 20 s late → still 10:15.
  CHECK_EQ(nominalSlot(kBase + 15 * kMinS + 20, kTz, 15), kBase + 15 * kMinS);
  // Interactive wake at 10:07 → slot 10:00.
  CHECK_EQ(nominalSlot(kBase + 7 * kMinS, kTz, 15), kBase);
  int h = 0;
  int m = 0;
  localHourMinute(kBase + 15 * kMinS, kTz, h, m);
  CHECK_EQ(h, 10);
  CHECK_EQ(m, 15);
}

TEST(battery_policy, next_wake_on_boundary_and_early_wake_tolerance) {
  WakePlan p = planNextWake(inputs(kBase + 20));  // work finished 10:00:20
  CHECK(p.kind == WakeKind::Boundary);
  CHECK_EQ(p.wake_unix, kBase + 15 * kMinS);
  CHECK_EQ(p.sleep_s, 15 * kMinS - 20);
  CHECK_EQ(p.interval_min, 15u);
  // Timer fired 50 s early (09:59:10) and work ended 09:59:30: next is 10:15, not 10:00.
  p = planNextWake(inputs(kBase - 30));
  CHECK_EQ(p.wake_unix, kBase + 15 * kMinS);
  // Interval options all align.
  for (uint32_t every : {5u, 10u, 15u, 30u, 60u}) {
    p = planNextWake(inputs(kBase + 7 * kMinS, every));
    CHECK_EQ((p.wake_unix - kMidnight) % (every * kMinS), 0u);
    CHECK(p.wake_unix > kBase + 7 * kMinS);
  }
}

TEST(battery_policy, low_battery_doubles_and_critical_stops_timer) {
  WakeInputs in = inputs(kBase + 20);
  in.battery_pct = 19;
  WakePlan p = planNextWake(in);
  CHECK_EQ(p.interval_min, 30u);
  CHECK_EQ(p.wake_unix, kBase + 30 * kMinS);
  in.battery_pct = 4;
  p = planNextWake(in);
  CHECK(p.kind == WakeKind::CriticalBattery);
  CHECK_EQ(p.sleep_s, 0u);
  // Charging never counts as critical; unknown battery is not scaled.
  in.charging = true;
  CHECK(planNextWake(in).kind == WakeKind::Boundary);
  in.charging = false;
  in.battery_known = false;
  CHECK_EQ(planNextWake(in).interval_min, 15u);
}

TEST(battery_policy, failure_backoff_is_capped) {
  CHECK_EQ(effectiveIntervalMin(15, true, 80, 0), 15u);
  CHECK_EQ(effectiveIntervalMin(15, true, 80, 1), 30u);
  CHECK_EQ(effectiveIntervalMin(15, true, 80, 2), 60u);
  CHECK_EQ(effectiveIntervalMin(15, true, 80, 3), 120u);
  CHECK_EQ(effectiveIntervalMin(15, true, 80, 9), 120u);
  CHECK_EQ(effectiveIntervalMin(60, true, 10, 9), kMaxIntervalMin);
  WakeInputs in = inputs(kBase + 20);
  in.consecutive_failures = 2;
  CHECK_EQ(planNextWake(in).wake_unix, kBase + kHourS);
}

TEST(battery_policy, night_window_pauses_until_end) {
  NightWindow night{0, 6 * 60};  // 00:00–06:00 local
  CHECK(night.enabled());
  uint64_t end = 0;
  CHECK(inNightWindow(kMidnight + 2 * kHourS, kTz, night, &end));
  CHECK_EQ(end, kMidnight + 6 * kHourS);
  CHECK(!inNightWindow(kMidnight + 6 * kHourS, kTz, night));
  // 23:50 with a 15-minute interval: next boundary 00:00 is inside → sleep to 06:00.
  WakeInputs in = inputs(kMidnight - 10 * kMinS);
  in.night = night;
  WakePlan p = planNextWake(in);
  CHECK(p.kind == WakeKind::NightPause);
  CHECK_EQ(p.wake_unix, kMidnight + 6 * kHourS);
  // 23:30 → 23:45 is still outside: normal boundary.
  in.now_unix = kMidnight - 30 * kMinS;
  p = planNextWake(in);
  CHECK(p.kind == WakeKind::Boundary);
  CHECK_EQ(p.wake_unix, kMidnight - 15 * kMinS);
  // Woken inside the window (power key at 02:00) → straight back to 06:00.
  in.now_unix = kMidnight + 2 * kHourS;
  p = planNextWake(in);
  CHECK(p.kind == WakeKind::NightPause);
  CHECK_EQ(p.sleep_s, 4 * kHourS);
  // Overnight window 22:00–06:00 from 23:00.
  NightWindow overnight{22 * 60, 6 * 60};
  CHECK(inNightWindow(kMidnight - kHourS, kTz, overnight, &end));
  CHECK_EQ(end, kMidnight + 6 * kHourS);
  CHECK(!(NightWindow{-1, -1}.enabled()));
  CHECK(!(NightWindow{60, 60}.enabled()));
}

TEST(battery_policy, no_clock_sleeps_relative) {
  WakeInputs in = inputs(12345);
  WakePlan p = planNextWake(in);
  CHECK(p.kind == WakeKind::NoClock);
  CHECK_EQ(p.sleep_s, 15 * kMinS);
  CHECK_EQ(p.wake_unix, 0u);
}

TEST(battery_policy, refresh_deadline_is_stable) {
  NightWindow none;
  // Refreshed at 10:00, max-stale 60 → screen updates by 11:00 at the latest.
  CHECK_EQ(refreshDeadline(kBase, 60, kTz, 15, none), kBase + kHourS);
  // Interval 45 (backoff): first wake boundary at/after 11:00 is 11:15.
  CHECK_EQ(refreshDeadline(kBase, 60, kTz, 45, none), kBase + 75 * kMinS);
  // Deadline inside the night window moves to the window end.
  NightWindow night{11 * 60, 12 * 60};
  CHECK_EQ(refreshDeadline(kBase, 60, kTz, 15, night), kBase + 2 * kHourS);
  CHECK_EQ(refreshDeadline(0, 60, kTz, 15, none), 0u);
}

TEST(battery_policy, unchanged_frame_is_skipped_until_stale) {
  const FrameRecord rec = record(0xabcULL, kBase);
  CHECK(decideRefresh(rec, 0xabcULL, kBase + 15 * kMinS, 60, true, false) == RefreshAction::Skip);
  CHECK(decideRefresh(rec, 0xabcULL, kBase + 45 * kMinS, 60, true, false) == RefreshAction::Skip);
  // 60 min (or within the early-wake tolerance of it) → forced redraw.
  CHECK(decideRefresh(rec, 0xabcULL, kBase + 60 * kMinS - 30, 60, true, false) ==
        RefreshAction::Fast);
  CHECK(decideRefresh(rec, 0xabcULL, kBase + 60 * kMinS, 60, true, false) == RefreshAction::Fast);
  // Changed content refreshes immediately.
  CHECK(decideRefresh(rec, 0xdefULL, kBase + 15 * kMinS, 60, true, false) == RefreshAction::Fast);
  // Unknown panel content / forced → Full.
  FrameRecord unknown;
  CHECK(decideRefresh(unknown, 0xabcULL, kBase, 60, true, false) == RefreshAction::Full);
  CHECK(decideRefresh(rec, 0xabcULL, kBase + 60, 60, true, true) == RefreshAction::Full);
  // Clock rolled back: never forced by staleness.
  CHECK(!frameStale(rec, kBase - kHourS, 60));
  CHECK(frameStale(rec, kBase, 0));
}

TEST(battery_policy, full_every_n_daily_and_missing_previous) {
  FrameRecord rec = record(1, kBase, 0, kBase);
  // Missing/corrupt stored frame (DTM1 not preloaded) → Full.
  CHECK(refreshMode(rec, kBase + 60, false, false) == RefreshAction::Full);
  int fulls = 0;
  uint64_t now = kBase;
  for (int i = 0; i < 24; ++i) {
    now += 15 * kMinS;
    const RefreshAction a = refreshMode(rec, now, true, false);
    if (a == RefreshAction::Full) ++fulls;
    noteRefreshed(rec, a, static_cast<uint64_t>(i) + 2, now, now);
  }
  CHECK_EQ(fulls, 2);  // every 12th refresh
  // Daily full even when fewer than N refreshes happened.
  rec = record(1, kBase, 1, kBase);
  CHECK(refreshMode(rec, kBase + 23 * kHourS, true, false) == RefreshAction::Fast);
  CHECK(refreshMode(rec, kBase + 24 * kHourS, true, false) == RefreshAction::Full);
  noteRefreshed(rec, RefreshAction::Full, 9, kBase + 24 * kHourS, kBase + 24 * kHourS);
  CHECK_EQ(rec.fast_since_full, 0u);
  CHECK_EQ(rec.full_unix, kBase + 24 * kHourS);
  CHECK_EQ(rec.hash, 9u);
  // Skip leaves the record untouched.
  const FrameRecord before = rec;
  noteRefreshed(rec, RefreshAction::Skip, 77, kBase, kBase);
  CHECK_EQ(rec.hash, before.hash);
}

TEST(battery_policy, battery_bucket_hysteresis) {
  CHECK_EQ(batteryDisplayBucket(78, -1), 75u);
  CHECK_EQ(batteryDisplayBucket(100, -1), 100u);
  CHECK_EQ(batteryDisplayBucket(3, -1), 0u);
  // Jitter around the 75/80 edge keeps the previous bucket.
  CHECK_EQ(batteryDisplayBucket(80, 75), 75u);
  CHECK_EQ(batteryDisplayBucket(81, 75), 75u);
  CHECK_EQ(batteryDisplayBucket(74, 75), 75u);
  CHECK_EQ(batteryDisplayBucket(73, 75), 75u);
  CHECK_EQ(batteryDisplayBucket(72, 75), 70u);
  CHECK_EQ(batteryDisplayBucket(82, 75), 80u);
}

TEST(battery_policy, packbits_and_frame_file_roundtrip) {
  std::vector<uint8_t> frame(52992, 0xff);
  for (size_t i = 0; i < frame.size(); i += 97) frame[i] = static_cast<uint8_t>(i * 31);
  for (size_t i = 1000; i < 1300; ++i) frame[i] = 0x00;
  const std::vector<uint8_t> packed = packbitsEncode(frame.data(), frame.size());
  CHECK(packed.size() < frame.size() / 4);
  std::vector<uint8_t> out(frame.size());
  CHECK(packbitsDecode(packed.data(), packed.size(), out.data(), out.size()));
  CHECK(out == frame);
  // Incompressible data still roundtrips.
  std::vector<uint8_t> noise(1000);
  for (size_t i = 0; i < noise.size(); ++i) noise[i] = static_cast<uint8_t>((i * 7919) >> 3);
  const std::vector<uint8_t> np = packbitsEncode(noise.data(), noise.size());
  std::vector<uint8_t> nout(noise.size());
  CHECK(packbitsDecode(np.data(), np.size(), nout.data(), nout.size()));
  CHECK(nout == noise);
  // Wrong output size is rejected.
  CHECK(!packbitsDecode(np.data(), np.size(), nout.data(), nout.size() - 1));

  const uint64_t hash = fnv1a64(frame.data(), frame.size());
  std::vector<uint8_t> file = encodeFrameFile(frame.data(), frame.size(), hash);
  uint64_t got = 0;
  std::fill(out.begin(), out.end(), 0);
  CHECK(decodeFrameFile(file.data(), file.size(), out.data(), out.size(), got));
  CHECK_EQ(got, hash);
  CHECK(out == frame);
  // Corruption anywhere is detected.
  std::vector<uint8_t> bad = file;
  bad[kFrameHeaderSize + 5] ^= 0x10;
  CHECK(!decodeFrameFile(bad.data(), bad.size(), out.data(), out.size(), got));
  bad = file;
  bad.pop_back();
  CHECK(!decodeFrameFile(bad.data(), bad.size(), out.data(), out.size(), got));
  CHECK(!decodeFrameFile(file.data(), 10, out.data(), out.size(), got));
  CHECK(!decodeFrameFile(file.data(), file.size(), out.data(), out.size() - 8, got));
}

TEST(battery_policy, rtc_state_checksum) {
  RtcState s;
  CHECK(!rtcValid(s));  // zeroed RTC memory after power-on
  rtcReset(s);
  CHECK(rtcValid(s));
  CHECK_EQ(s.battery_bucket, -1);
  s.frame = record(5, kBase);
  s.ap_valid = 1;
  s.ap_channel = 6;
  std::strcpy(s.ap_ssid, "office");
  CHECK(!rtcValid(s));
  rtcSeal(s);
  CHECK(rtcValid(s));
  s.consecutive_failures = 3;
  CHECK(!rtcValid(s));
  rtcSeal(s);
  CHECK_EQ(s.manual_sleep, 0);  // 电源键休眠标志默认关
  s.manual_sleep = 1;
  CHECK(!rtcValid(s));
  rtcSeal(s);
  CHECK(rtcValid(s));
  CHECK_EQ(crc32(reinterpret_cast<const uint8_t*>("123456789"), 9), 0xCBF43926u);
}

TEST(battery_policy, mains_detection) {
  CHECK(mainsPowered(1, false, 3700));
  CHECK(!mainsPowered(2, true, 3700));
  CHECK(mainsPowered(0, false, 0));
  CHECK(mainsPowered(0, true, 3900));
  CHECK(!mainsPowered(0, false, 3900));
}
