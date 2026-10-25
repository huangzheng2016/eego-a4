// SPDX-License-Identifier: GPL-3.0-or-later
// Keep-awake ("config mode") override: timing helpers (admin/keep_awake.h), its
// interplay with the idle refresh cadence (refresh_policy) and battery sleep gating
// (battery_policy), plus the mains/battery source detector with latch + hysteresis.
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

TEST(power_source, charging_and_usb_are_always_mains) {
  bat::MainsDetector d;
  bat::PowerSample s;
  s.battery_mv = 3800;
  s.charging = true;
  CHECK(d.update(0, s, 0));
  CHECK(d.reason() == bat::PowerReason::Charging);
  s.charging = false;
  s.usb_host = true;
  CHECK(d.update(0, s, 1000));
  CHECK(d.reason() == bat::PowerReason::UsbHost);
  s.usb_host = false;
  s.battery_mv = 0;
  CHECK(d.update(0, s, 2000));
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

TEST(power_source, full_battery_on_usb_stays_mains) {
  bat::MainsDetector d;
  bat::PowerSample s;
  s.battery_mv = 4120;
  s.charging = true;
  CHECK(d.update(0, s, 0));
  // Charger terminates: CHG pin goes inactive, voltage settles near full.
  s.charging = false;
  s.battery_mv = 4180;
  int64_t t = 60000;
  for (int i = 0; i < 120; ++i, t += 60000) {  // two hours, ±20 mV ADC noise
    s.battery_mv = static_cast<uint16_t>(4170 + (i % 3) * 10 - 10);
    CHECK(d.update(0, s, t));
  }
  CHECK(d.reason() == bat::PowerReason::FullHold);
  // Recharge cycle restarts: still mains.
  s.charging = true;
  s.battery_mv = 4100;
  CHECK(d.update(0, s, t));
}

TEST(power_source, unplug_detected_within_minutes) {
  bat::MainsDetector d;
  bat::PowerSample s;
  s.charging = true;
  s.battery_mv = 4190;
  d.update(0, s, 0);
  s.charging = false;
  d.update(0, s, 60000);
  // Unplugged at t=2 min: voltage sags under WiFi load ~10 mV/min.
  int64_t detected = -1;
  for (int i = 0; i <= 15; ++i) {
    const int64_t t = 120000 + i * 60000;
    s.battery_mv = static_cast<uint16_t>(4190 - 10 * i);
    if (!d.update(0, s, t)) {
      detected = t;
      break;
    }
  }
  CHECK(detected > 0);
  CHECK(detected <= 120000 + 8 * 60000);
  CHECK(d.reason() == bat::PowerReason::VoltageDrop);
  // Unplugged mid-charge (below the hold voltage) → battery immediately.
  bat::MainsDetector e;
  s.charging = true;
  s.battery_mv = 3950;
  CHECK(e.update(0, s, 0));
  s.charging = false;
  s.battery_mv = 3930;
  CHECK(!e.update(0, s, 1000));
  CHECK(e.reason() == bat::PowerReason::BelowHold);
}

TEST(power_source, cold_boot_seed_and_plain_battery) {
  bat::PowerSample s;
  s.battery_mv = 4150;
  bat::MainsDetector seeded;
  seeded.seedColdBoot(4150, 0);
  CHECK(seeded.update(0, s, 0));
  CHECK(seeded.reason() == bat::PowerReason::FullHold);
  bat::MainsDetector plain;
  CHECK(!plain.update(0, s, 0));
  CHECK(plain.reason() == bat::PowerReason::Battery);
  bat::MainsDetector low;
  low.seedColdBoot(3900, 0);
  s.battery_mv = 3900;
  CHECK(!low.update(0, s, 0));
}
