// SPDX-License-Identifier: GPL-3.0-or-later
// main/input_confirm: dangerous-action confirmation = a fresh short press of the
// same control (or physical Power) inside the 5 s window after arming.
#include "input_confirm.h"
#include "test_framework.h"

using namespace panel;

namespace {

constexpr int64_t kArm = 10000000;  // arm time (us)
constexpr int64_t kMs = 1000;

ConfirmTarget target(const char* control, bool physical = false) {
  ConfirmTarget t;
  t.page_epoch = 3;
  t.control = control;
  t.physical_power_only = physical;
  return t;
}

}  // namespace

TEST(confirm, fresh_short_press_confirms) {
  FreshPressConfirm c;
  c.arm(target("soft:1"), kArm);
  CHECK(c.armed());
  CHECK(!c.pressing());
  CHECK(c.beginPress(ConfirmSource::Touch, "soft:1", kArm + 500 * kMs, kArm + 500 * kMs));
  CHECK(c.pressing());
  CHECK(!c.completePress(ConfirmSource::Touch, kArm + 550 * kMs));  // not released yet
  c.noteRelease(ConfirmSource::Touch, kArm + 600 * kMs);
  CHECK(!c.completePress(ConfirmSource::Up, kArm + 600 * kMs));  // other source
  CHECK(c.completePress(ConfirmSource::Touch, kArm + 600 * kMs));
  c.cancel();
  CHECK(!c.armed());
  CHECK(!c.completePress(ConfirmSource::Touch, kArm + 600 * kMs));
}

TEST(confirm, press_started_before_arming_does_not_confirm) {
  FreshPressConfirm c;
  c.arm(target("trigger:up"), kArm);
  CHECK(!c.beginPress(ConfirmSource::Up, "trigger:up", kArm - 1, kArm + 100 * kMs));
  CHECK(!c.beginPress(ConfirmSource::Up, "trigger:up", kArm, kArm + 100 * kMs));
  CHECK(!c.pressing());
  c.noteRelease(ConfirmSource::Up, kArm + 100 * kMs);
  CHECK(!c.completePress(ConfirmSource::Up, kArm + 100 * kMs));
}

TEST(confirm, wrong_control_is_rejected) {
  FreshPressConfirm c;
  c.arm(target("row:2"), kArm);
  CHECK(!c.beginPress(ConfirmSource::Touch, "row:3", kArm + kMs, kArm + kMs));
  CHECK(!c.beginPress(ConfirmSource::Touch, "", kArm + kMs, kArm + kMs));
  CHECK(!c.pressing());
  // A second press while one is in flight (e.g. chord) is rejected too.
  CHECK(c.beginPress(ConfirmSource::Up, "row:2", kArm + 2 * kMs, kArm + 2 * kMs));
  CHECK(!c.beginPress(ConfirmSource::Down, "row:2", kArm + 3 * kMs, kArm + 3 * kMs));
}

TEST(confirm, physical_power_only_requires_power) {
  FreshPressConfirm c;
  c.arm(target("settings.factory_reset", true), kArm);
  CHECK(!c.beginPress(ConfirmSource::Touch, "settings.factory_reset", kArm + kMs, kArm + kMs));
  CHECK(!c.beginPress(ConfirmSource::Up, "settings.factory_reset", kArm + kMs, kArm + kMs));
  CHECK(c.beginPress(ConfirmSource::Power, "settings.factory_reset", kArm + kMs, kArm + kMs));
  c.noteRelease(ConfirmSource::Power, kArm + 100 * kMs);
  CHECK(c.completePress(ConfirmSource::Power, kArm + 100 * kMs));
}

TEST(confirm, window_expiry) {
  FreshPressConfirm c;
  c.arm(target("soft:0"), kArm);
  CHECK(!c.expired(kArm + kConfirmWindowUs));
  CHECK(c.expired(kArm + kConfirmWindowUs + 1));
  CHECK(!c.beginPress(ConfirmSource::Touch, "soft:0", kArm + kConfirmWindowUs + 1,
                      kArm + kConfirmWindowUs + 1));
  // Pressed in time but the short action lands after the window: rejected.
  c.arm(target("soft:0"), kArm);
  const int64_t press = kArm + kConfirmWindowUs - 100 * kMs;
  CHECK(c.beginPress(ConfirmSource::Touch, "soft:0", press, press));
  c.noteRelease(ConfirmSource::Touch, press + 50 * kMs);
  CHECK(!c.completePress(ConfirmSource::Touch, kArm + kConfirmWindowUs + 1));
}

TEST(confirm, long_press_does_not_confirm) {
  FreshPressConfirm c;
  c.arm(target("trigger:down"), kArm);
  CHECK(c.beginPress(ConfirmSource::Down, "trigger:down", kArm + kMs, kArm + kMs));
  c.noteRelease(ConfirmSource::Down, kArm + kMs + eego::kInputLongPressUs);
  CHECK(!c.completePress(ConfirmSource::Down, kArm + kMs + eego::kInputLongPressUs));
  // Just under the long-press threshold still counts as a short press.
  c.arm(target("trigger:down"), kArm);
  CHECK(c.beginPress(ConfirmSource::Down, "trigger:down", kArm + kMs, kArm + kMs));
  c.noteRelease(ConfirmSource::Down, kArm + kMs + eego::kInputLongPressUs - 1);
  CHECK(c.completePress(ConfirmSource::Down, kArm + kMs + eego::kInputLongPressUs));
}

TEST(confirm, release_from_other_source_is_ignored) {
  FreshPressConfirm c;
  c.arm(target("trigger:up"), kArm);
  CHECK(c.beginPress(ConfirmSource::Up, "trigger:up", kArm + kMs, kArm + kMs));
  c.noteRelease(ConfirmSource::Power, kArm + 50 * kMs);
  CHECK(!c.completePress(ConfirmSource::Up, kArm + 60 * kMs));
  c.noteRelease(ConfirmSource::Up, kArm + 70 * kMs);
  CHECK(c.completePress(ConfirmSource::Up, kArm + 70 * kMs));
}

TEST(confirm, record_target_must_stay_current) {
  ConfirmTarget t = target("detail.mute");
  t.snapshot_epoch = 7;
  t.record_id = "r1";
  alert_core::AlertSnapshot snap;
  snap.seq = 7;
  alert_core::AlertRecord rec;
  rec.id = "r1";
  rec.state = alert_core::AlertState::Active;
  snap.records.push_back(rec);
  CHECK(confirmRecordTargetCurrent(t, ConfirmRecordRule::Mute, &snap));
  CHECK(confirmRecordTargetCurrent(t, ConfirmRecordRule::Clear, &snap));
  snap.records[0].muted = true;
  CHECK(!confirmRecordTargetCurrent(t, ConfirmRecordRule::Mute, &snap));
  snap.seq = 8;
  CHECK(!confirmRecordTargetCurrent(t, ConfirmRecordRule::Clear, &snap));
  CHECK(!confirmRecordTargetCurrent(t, ConfirmRecordRule::Clear, nullptr));
}
