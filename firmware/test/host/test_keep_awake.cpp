// SPDX-License-Identifier: GPL-3.0-or-later
// Keep-awake ("config mode") override: timing helpers (admin/keep_awake.h), its
// interplay with the idle refresh cadence (refresh_policy) and battery sleep gating
// (battery_policy), plus the mains/battery source detector with latch + hysteresis.
#include "admin/device_screen.h"
#include "admin/keep_awake.h"
#include "battery_policy.h"
#include "refresh_policy.h"
#include "test_framework.h"

namespace ka = eego::admin::keep_awake;
using panel::RefreshPolicy;
using panel::RefreshPolicyConfig;
namespace bat = panel::battery;

namespace {

constexpr int64_t kMin = 60000;
// 2026-10-25 02:00:00 UTC.
constexpr uint64_t kUnix = 1792893600ULL;

RefreshPolicy idlePolicy() {
  RefreshPolicy p;
  RefreshPolicyConfig c;
  c.idle_after_min = 10;
  c.idle_refresh_min = 5;
  c.timezone_offset_min = 480;
  p.configure(c);
  return p;
}

}  // namespace

TEST(keep_awake, timing_helpers) {
  CHECK_EQ(ka::deadlineMs(1000, 0), 0);
  CHECK_EQ(ka::deadlineMs(1000, 30), 1000 + 30 * kMin);
  CHECK(ka::active(1000, 2000));
  CHECK(!ka::active(2000, 2000));
  CHECK(!ka::active(5, 0));
  CHECK_EQ(ka::remainingS(0, 30 * kMin), 1800u);
  CHECK_EQ(ka::remainingS(1, 30 * kMin), 1800u);  // rounds up
  CHECK_EQ(ka::remainingS(30 * kMin, 30 * kMin), 0u);
  CHECK(ka::validRequestMinutes(0));
  CHECK(ka::validRequestMinutes(240));
  CHECK(!ka::validRequestMinutes(241));
  CHECK(!ka::validRequestMinutes(-1));
  CHECK(ka::validSettingMinutes(30));
  CHECK(!ka::validSettingMinutes(45));
}

TEST(keep_awake, display_minutes_are_coarse_above_ten) {
  CHECK_EQ(ka::displayMinutes(0), 0u);
  CHECK_EQ(ka::displayMinutes(1), 1u);
  CHECK_EQ(ka::displayMinutes(60), 1u);
  CHECK_EQ(ka::displayMinutes(61), 2u);
  CHECK_EQ(ka::displayMinutes(600), 10u);
  CHECK_EQ(ka::displayMinutes(601), 15u);
  CHECK_EQ(ka::displayMinutes(1800), 30u);
  CHECK_EQ(ka::displayMinutes(1741), 30u);
  CHECK_EQ(ka::displayMinutes(1500), 25u);
}

TEST(keep_awake, toggle_starts_extends_or_cancels) {
  CHECK_EQ(ka::toggleMinutes(0, 30), 30u);           // off → on
  CHECK_EQ(ka::toggleMinutes(600, 30), 30u);         // < half left → extend
  CHECK_EQ(ka::toggleMinutes(900, 30), 30u);         // exactly half → extend
  CHECK_EQ(ka::toggleMinutes(1700, 30), 0u);         // > half left → cancel
  CHECK_EQ(ka::toggleMinutes(0, 0), ka::kDefaultMinutes);
}

TEST(keep_awake, blocks_idle_cadence_and_expiry_restores_it) {
  RefreshPolicy p = idlePolicy();
  p.noteInteraction(0);
  // Keep awake for 30 min from t=1 min.
  p.setHoldUntil(ka::deadlineMs(1 * kMin, 30));
  CHECK(!p.idle(20 * kMin));                    // would be idle (>10 min) without the hold
  CHECK(p.allow(20 * kMin, kUnix + 60, false));  // updates display immediately
  p.noteRendered(kUnix + 60);
  CHECK(!p.idle(31 * kMin - 1));
  // Expired: last interaction was long ago → idle cadence again right away.
  CHECK(p.idle(31 * kMin));
  CHECK(!p.allow(31 * kMin, kUnix + 120, false));  // deferred to the next wall-clock boundary
  CHECK(p.deferred());
  // Cancel (0) behaves like expiry.
  p.setHoldUntil(ka::deadlineMs(40 * kMin, 30));
  CHECK(!p.idle(41 * kMin));
  p.setHoldUntil(0);
  CHECK(p.idle(41 * kMin));
}

TEST(keep_awake, hold_does_not_shorten_normal_activity) {
  RefreshPolicy p = idlePolicy();
  p.noteInteraction(25 * kMin);
  p.setHoldUntil(ka::deadlineMs(0, 30));  // expires at 30 min
  CHECK(!p.idle(32 * kMin));              // recent interaction still keeps it active
  CHECK(p.idle(35 * kMin));
}

TEST(keep_awake, blocks_battery_interactive_sleep) {
  const uint32_t timeout_s = 60;
  CHECK(bat::interactiveSleepDue(61000, 0, timeout_s, 0));
  CHECK(!bat::interactiveSleepDue(59000, 0, timeout_s, 0));
  const int64_t until = ka::deadlineMs(0, 10);
  CHECK(!bat::interactiveSleepDue(5 * kMin, 0, timeout_s, until));
  CHECK(!bat::interactiveSleepDue(10 * kMin - 1, 0, timeout_s, until));
  // Expired: no grace — sleeps as soon as the timeout since the last activity elapsed.
  CHECK(bat::interactiveSleepDue(10 * kMin, 0, timeout_s, until));
  CHECK(!bat::interactiveSleepDue(10 * kMin, 10 * kMin - 30000, timeout_s, until));
  CHECK(bat::keepAwakeBlocks(1, 2));
  CHECK(!bat::keepAwakeBlocks(2, 2));
  CHECK(!bat::keepAwakeBlocks(2, 0));
}

namespace {

constexpr int64_t kSec = 1000;

// Feeds samples every `step` ms; returns the first time the detector says battery
// (or -1). `mv(t)` gives the voltage at absolute time t.
template <typename Mv>
int64_t runUntilBattery(bat::MainsDetector& d, bat::PowerSample s, int64_t from, int64_t to,
                        int64_t step, Mv mv) {
  for (int64_t t = from; t <= to; t += step) {
    s.battery_mv = mv(t);
    s.battery_read_ms = t;
    if (!d.update(0, s, t)) return t;
  }
  return -1;
}

// Deterministic ±10 mV ADC noise.
uint16_t noisy(int base, int64_t t) {
  const uint32_t x = static_cast<uint32_t>(t / 1000) * 2654435761u;
  return static_cast<uint16_t>(base + static_cast<int>((x >> 16) % 21) - 10);
}

}  // namespace

TEST(power_source, charging_and_usb_are_always_mains) {
  bat::MainsDetector d;
  bat::PowerSample s;
  s.battery_mv = 3800;
  s.charging = true;
  CHECK(d.update(0, s, 0));
  CHECK(d.reason() == bat::PowerReason::Charging);
  s.usb_host = true;
  CHECK(d.update(0, s, 1000));
  CHECK(d.reason() == bat::PowerReason::UsbHost);  // charging shown in the battery row
  s.charging = false;
  CHECK(d.update(0, s, 2000));
  CHECK(d.reason() == bat::PowerReason::UsbHost);
  s.usb_host = false;
  s.battery_mv = 0;
  CHECK(d.update(0, s, 3000));
  CHECK(d.reason() == bat::PowerReason::NoBattery);
}

TEST(power_source, forced_modes) {
  bat::MainsDetector d;
  bat::PowerSample s;
  s.battery_mv = 3900;
  CHECK(d.update(1, s, 0));
  CHECK(d.reason() == bat::PowerReason::ForcedMains);
  s.charging = true;
  CHECK(!d.update(2, s, 0));
  CHECK(d.reason() == bat::PowerReason::ForcedBattery);
  CHECK_EQ(std::string(bat::powerReasonName(bat::PowerReason::FullHold)), std::string("full_hold"));
}

TEST(power_source, unplug_from_host_is_battery_within_one_sample) {
  // Full battery on a computer: CHG inactive, only the USB host is evidence.
  bat::MainsDetector d;
  bat::PowerSample s;
  s.usb_host = true;
  s.battery_mv = 4190;
  for (int64_t t = 0; t < 30 * kMin; t += 5 * kSec) CHECK(d.update(0, s, t));
  CHECK(d.reason() == bat::PowerReason::UsbHost);
  s.usb_host = false;  // unplugged: voltage still at full
  CHECK(!d.update(0, s, 30 * kMin));
  CHECK(d.reason() == bat::PowerReason::Battery);
  // Charging on the computer, then unplugged: no charge debounce delay either.
  bat::MainsDetector e;
  s.usb_host = true;
  s.charging = true;
  s.battery_mv = 4170;
  CHECK(e.update(0, s, 0));
  s.usb_host = false;
  s.charging = false;
  CHECK(!e.update(0, s, 5 * kSec));
  // Charging finished while on the computer: no voltage latch after unplug.
  bat::MainsDetector f;
  s.usb_host = true;
  s.charging = true;
  f.update(0, s, 0);
  s.charging = false;
  for (int64_t t = 5 * kSec; t < 5 * kMin; t += 5 * kSec) CHECK(f.update(0, s, t));
  s.usb_host = false;
  CHECK(!f.update(0, s, 5 * kMin));
  CHECK(!f.latched());
}

TEST(power_source, wall_charger_full_battery_stays_external_with_recharge_cycles) {
  bat::MainsDetector d;
  bat::PowerSample s;
  s.charging = true;
  s.battery_mv = 4200;
  CHECK(d.update(0, s, 0));
  CHECK(d.wantsFastSamples());
  // Two hours: charger idle for 15 min (voltage relaxes 4200 → ~4170 with ±10 mV
  // noise), then a 1-minute top-up, repeat.
  int64_t t = 10 * kSec;
  const int64_t step = 10 * kSec;
  while (t < 2 * 60 * kMin) {
    const int64_t idle_end = t + 15 * kMin;
    const int64_t idle_start = t;
    for (; t < idle_end; t += step) {
      s.charging = false;
      const int64_t since = t - idle_start;
      const int base = since < 2 * kMin ? 4200 - static_cast<int>(since / (4 * kSec)) : 4170;
      s.battery_mv = noisy(base, t);
      s.battery_read_ms = t;
      CHECK(d.update(0, s, t));
    }
    CHECK(d.reason() == bat::PowerReason::FullHold);
    for (const int64_t end = t + kMin; t < end; t += step) {
      s.charging = true;
      s.battery_mv = noisy(4195, t);
      s.battery_read_ms = t;
      CHECK(d.update(0, s, t));
    }
  }
  // No top-up for longer than 20 min → no longer trusted as external power.
  bat::MainsDetector e;
  s.charging = true;
  s.battery_mv = 4190;
  e.update(0, s, 0);
  s.charging = false;
  const int64_t gone = runUntilBattery(e, s, 10 * kSec, 30 * kMin, 10 * kSec,
                                       [](int64_t) -> uint16_t { return 4175; });
  CHECK(gone > 20 * kMin);
  CHECK(gone <= 20 * kMin + 20 * kSec);
  CHECK(e.reason() == bat::PowerReason::Battery);
}

TEST(power_source, unplug_from_wall_charger_with_full_battery) {
  bat::MainsDetector d;
  bat::PowerSample s;
  s.charging = true;
  s.battery_mv = 4195;
  d.update(0, s, 0);
  s.charging = false;
  // Holding at full for 5 minutes.
  CHECK(runUntilBattery(d, s, 10 * kSec, 5 * kMin, 10 * kSec,
                        [](int64_t t) { return noisy(4175, t); }) < 0);
  CHECK(d.reason() == bat::PowerReason::FullHold);
  // Unplugged at 5 min: load moves to the battery, ~10 mV/min decline (WiFi on).
  const int64_t unplug = 5 * kMin;
  const int64_t found =
      runUntilBattery(d, s, unplug + 10 * kSec, unplug + 20 * kMin, 10 * kSec, [&](int64_t t) {
        return noisy(4175 - static_cast<int>((t - unplug) / 6000), t);
      });
  CHECK(found > 0);
  CHECK(found - unplug <= 3 * kMin + 30 * kSec);
  CHECK(d.reason() == bat::PowerReason::VoltageDrop);
  // A single low ADC spike does not release the latch (median filter).
  bat::MainsDetector e;
  s.charging = true;
  e.update(0, s, 0);
  s.charging = false;
  CHECK(runUntilBattery(e, s, 10 * kSec, 10 * kMin, 10 * kSec, [](int64_t t) -> uint16_t {
          return t == 5 * kMin ? 4100 : 4175;
        }) < 0);
  // Unplugged mid-charge (below the hold voltage) → battery once the charge pin settles.
  bat::MainsDetector f;
  s.charging = true;
  s.battery_mv = 3950;
  s.battery_read_ms = -1;
  CHECK(f.update(0, s, 0));
  s.charging = false;
  s.battery_mv = 3930;
  CHECK(f.update(0, s, 5 * kSec));  // charge pin debounce (10 s)
  CHECK(!f.update(0, s, 15 * kSec));
  CHECK(f.reason() == bat::PowerReason::BelowHold);
}

TEST(power_source, boot_does_not_latch_from_voltage_alone) {
  bat::PowerSample s;
  s.battery_mv = 4190;
  bat::MainsDetector plain;
  CHECK(!plain.update(0, s, 0));
  CHECK(plain.reason() == bat::PowerReason::Battery);
  CHECK(!plain.update(0, s, 60 * kSec));
  // Charging seen during boot → latches once the charger tops off.
  bat::MainsDetector charged;
  s.charging = true;
  CHECK(charged.update(0, s, 0));
  s.charging = false;
  CHECK(charged.update(0, s, 5 * kSec));
  CHECK(charged.update(0, s, 30 * kSec));
  CHECK(charged.reason() == bat::PowerReason::FullHold);
}

// ---- Multi-screen dashboard: remote screen target + carousel timing ----

TEST(device_screen, resolve_index_and_wrapping_step) {
  namespace ds = eego::admin::device_screen;
  CHECK_EQ(ds::resolveTarget(true, 2, 0, 0, 5), int64_t{2});
  CHECK_EQ(ds::resolveTarget(true, 5, 0, 0, 5), int64_t{-1});
  CHECK_EQ(ds::resolveTarget(true, -1, 0, 0, 5), int64_t{-1});
  CHECK_EQ(ds::resolveTarget(false, 0, 1, 4, 5), int64_t{0});   // last -> first
  CHECK_EQ(ds::resolveTarget(false, 0, -1, 0, 5), int64_t{4});  // first -> last
  CHECK_EQ(ds::resolveTarget(false, 0, 2, 1, 5), int64_t{3});
  CHECK_EQ(ds::resolveTarget(false, 0, 0, 1, 5), int64_t{-1});
  CHECK_EQ(ds::resolveTarget(false, 0, 9, 1, 5), int64_t{-1});
  CHECK_EQ(ds::resolveTarget(false, 0, 1, 0, 0), int64_t{-1});  // no dashboard
}

TEST(screen_rotate, wall_clock_epochs_and_gating) {
  CHECK_EQ(panel::screenRotateEpoch(kUnix, 0, 0, 0), uint64_t{0});  // off
  const uint64_t e = panel::screenRotateEpoch(kUnix, 0, 0, 5);
  CHECK(e != 0);
  CHECK_EQ(panel::screenRotateEpoch(kUnix + 299, 0, 0, 5), e);
  CHECK_EQ(panel::screenRotateEpoch(kUnix + 300, 0, 0, 5), e + 1);
  // Untrusted wall clock: monotonic fallback.
  CHECK_EQ(panel::screenRotateEpoch(0, 4 * kMin, 0, 5), uint64_t{1});
  CHECK_EQ(panel::screenRotateEpoch(0, 5 * kMin, 0, 5), uint64_t{2});

  const int64_t now = 100 * kMin;
  // Due: new epoch, idle for a full period, previous rotation displayed.
  CHECK(panel::screenRotateDue(e + 1, e, now, now - 5 * kMin, 5, true));
  CHECK(!panel::screenRotateDue(e, e, now, now - 5 * kMin, 5, true));      // same period
  CHECK(!panel::screenRotateDue(e + 1, 0, now, now - 5 * kMin, 5, true));  // first sample
  CHECK(!panel::screenRotateDue(e + 1, e, now, now - 4 * kMin, 5, true));  // recent input
  CHECK(!panel::screenRotateDue(e + 1, e, now, now - 5 * kMin, 5, false)); // not shown yet
  CHECK(!panel::screenRotateDue(e + 1, e, now, now - 5 * kMin, 0, true));  // disabled
}
