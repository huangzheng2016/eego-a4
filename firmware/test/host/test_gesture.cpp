// SPDX-License-Identifier: GPL-3.0-or-later
// eego_hal input_gesture: touch swipe/tap/hold/front FSM and button FSM.
#include <initializer_list>

#include "input_gesture.h"
#include "test_framework.h"

using namespace eego;

namespace {

constexpr int64_t kMs = 1000;

struct TouchRun {
  TouchGestureFsm fsm;
  std::vector<TouchEvent> events;
  int64_t now = 1000 * kMs;

  void at(TouchContact c, int x, int y, int64_t advance_ms = 10) {
    now += advance_ms * kMs;
    auto out = fsm.sample(c, static_cast<uint16_t>(x), static_cast<uint16_t>(y), now);
    events.insert(events.end(), out.begin(), out.end());
  }
  void tick(int64_t advance_ms) {
    now += advance_ms * kMs;
    auto out = fsm.tick(now);
    events.insert(events.end(), out.begin(), out.end());
  }
  void up(int64_t advance_ms = 10) { at(TouchContact::None, 0, 0, advance_ms); }
  int count(TouchEventType type) const {
    int n = 0;
    for (const auto& e : events) n += e.type == type;
    return n;
  }
  bool any(std::initializer_list<TouchEventType> types) const {
    for (auto t : types)
      if (count(t) != 0) return true;
    return false;
  }
};

// Press at (x0,y0), drag to (x1,y1) over `ms`, release.
TouchRun swipe(int x0, int y0, int x1, int y1, int64_t ms = 100) {
  TouchRun run;
  run.at(TouchContact::Screen, x0, y0);
  run.at(TouchContact::Screen, (x0 + x1) / 2, (y0 + y1) / 2, ms / 2);
  run.at(TouchContact::Screen, x1, y1, ms / 2);
  run.up();
  return run;
}

const auto kSwipes = {TouchEventType::SwipeLeft, TouchEventType::SwipeRight,
                      TouchEventType::SwipeUp, TouchEventType::SwipeDown};

}  // namespace

TEST(gesture, swipe_directions) {
  CHECK_EQ(swipe(300, 400, 200, 400).count(TouchEventType::SwipeLeft), 1);
  CHECK_EQ(swipe(200, 400, 300, 400).count(TouchEventType::SwipeRight), 1);
  CHECK_EQ(swipe(300, 400, 300, 300).count(TouchEventType::SwipeUp), 1);    // y decreases
  CHECK_EQ(swipe(300, 300, 300, 400).count(TouchEventType::SwipeDown), 1);  // y increases
  const TouchRun r = swipe(300, 300, 300, 400);
  CHECK_EQ(r.count(TouchEventType::Tap), 0);
  CHECK_EQ(r.count(TouchEventType::Press), 1);
  CHECK_EQ(r.count(TouchEventType::Release), 1);
}

TEST(gesture, swipe_minimum_distance_is_60px) {
  CHECK_EQ(swipe(300, 400, 240, 400).count(TouchEventType::SwipeLeft), 1);  // exactly 60
  const TouchRun short_drag = swipe(300, 400, 241, 400);                    // 59
  CHECK(!short_drag.any(kSwipes));
  CHECK_EQ(short_drag.count(TouchEventType::Tap), 0);  // moved beyond slop -> not a tap either
  CHECK_EQ(swipe(300, 400, 300, 460).count(TouchEventType::SwipeDown), 1);
  CHECK(!swipe(300, 400, 300, 459).any(kSwipes));
}

TEST(gesture, swipe_direction_ratio_is_1_5) {
  // Major >= 1.5 * minor is required.
  CHECK_EQ(swipe(200, 400, 290, 460).count(TouchEventType::SwipeRight), 1);  // 90 vs 60
  CHECK(!swipe(200, 400, 290, 461).any(kSwipes));                            // 90 vs 61
  CHECK_EQ(swipe(300, 400, 240, 310).count(TouchEventType::SwipeUp), 1);     // 90 vs 60
  CHECK(!swipe(300, 400, 300 - 80, 400 - 80).any(kSwipes));                  // 45 degrees
}

TEST(gesture, slow_drag_is_not_a_swipe) {
  CHECK(!swipe(300, 400, 150, 400, 1400).any(kSwipes));  // > 1.2 s
  CHECK_EQ(swipe(300, 400, 150, 400, 1100).count(TouchEventType::SwipeLeft), 1);
}

TEST(gesture, jitter_within_slop_still_taps) {
  TouchRun run;
  run.at(TouchContact::Screen, 100, 100);
  run.at(TouchContact::Screen, 110, 105);
  run.at(TouchContact::Screen, 124, 76);  // exactly at the 24 px slop
  run.up();
  CHECK_EQ(run.count(TouchEventType::Move), 0);
  CHECK_EQ(run.count(TouchEventType::Tap), 1);
  CHECK(!run.any(kSwipes));
}

TEST(gesture, move_only_after_slop) {
  TouchRun run;
  run.at(TouchContact::Screen, 100, 100);
  run.at(TouchContact::Screen, 125, 100);
  CHECK_EQ(run.count(TouchEventType::Move), 1);
  run.at(TouchContact::Screen, 126, 100);
  CHECK_EQ(run.count(TouchEventType::Move), 2);
  run.at(TouchContact::Screen, 126, 100);  // same point: no event
  CHECK_EQ(run.count(TouchEventType::Move), 2);
}

TEST(gesture, hold_and_no_tap_after_hold) {
  TouchRun run;
  run.at(TouchContact::Screen, 200, 200);
  run.tick(650);
  CHECK_EQ(run.count(TouchEventType::Hold), 0);
  run.tick(60);
  CHECK_EQ(run.count(TouchEventType::Hold), 1);
  run.tick(500);
  CHECK_EQ(run.count(TouchEventType::Hold), 1);  // only once
  run.up();
  CHECK_EQ(run.count(TouchEventType::Tap), 0);
  CHECK(!run.any(kSwipes));
  // Jitter inside slop does not prevent hold-to-confirm.
  TouchRun jitter;
  jitter.at(TouchContact::Screen, 200, 200);
  jitter.at(TouchContact::Screen, 210, 190, 400);
  jitter.at(TouchContact::Screen, 205, 195, 400);
  CHECK_EQ(jitter.count(TouchEventType::Hold), 1);
  // Movement beyond slop cancels the hold.
  TouchRun dragged;
  dragged.at(TouchContact::Screen, 200, 200);
  dragged.at(TouchContact::Screen, 260, 200, 100);
  dragged.tick(900);
  CHECK_EQ(dragged.count(TouchEventType::Hold), 0);
}

TEST(gesture, front_short_double_long) {
  TouchRun shortp;
  shortp.at(TouchContact::Front, 0, 0);
  shortp.up(50);
  CHECK_EQ(shortp.count(TouchEventType::FrontShort), 0);  // waits for possible double
  shortp.tick(270);
  CHECK_EQ(shortp.count(TouchEventType::FrontShort), 0);
  shortp.tick(20);
  CHECK_EQ(shortp.count(TouchEventType::FrontShort), 1);

  TouchRun dbl;
  dbl.at(TouchContact::Front, 0, 0);
  dbl.up(50);
  dbl.at(TouchContact::Front, 0, 0, 100);
  dbl.up(50);
  dbl.tick(1000);
  CHECK_EQ(dbl.count(TouchEventType::FrontDouble), 1);
  CHECK_EQ(dbl.count(TouchEventType::FrontShort), 0);

  TouchRun lng;
  lng.at(TouchContact::Front, 0, 0);
  lng.tick(710);
  lng.up();
  lng.tick(1000);
  CHECK_EQ(lng.count(TouchEventType::FrontLong), 1);
  CHECK_EQ(lng.count(TouchEventType::FrontShort), 0);
}

TEST(gesture, contact_switch_and_invalidate_cancel) {
  TouchRun run;
  run.at(TouchContact::Screen, 100, 100);
  run.at(TouchContact::Front, 0, 0);
  CHECK_EQ(run.count(TouchEventType::Cancel), 1);
  CHECK_EQ(run.count(TouchEventType::FrontPress), 1);
  auto out = run.fsm.invalidate(run.now);
  CHECK(out.size() == 1 && out[0].type == TouchEventType::Cancel);
  CHECK(!run.fsm.active());
  CHECK(run.fsm.invalidate(run.now).empty());  // nothing pending -> no event
}

namespace {

struct ButtonRun {
  ButtonGestureFsm fsm;
  std::vector<ButtonEvent> events;
  int64_t now = 5000 * kMs;
  ButtonRun() { fsm.reset(now, 0, false); }

  void set(ButtonId id, bool pressed, int64_t advance_ms = 0) {
    now += advance_ms * kMs;
    auto out = fsm.sample(id, pressed, now);
    events.insert(events.end(), out.begin(), out.end());
  }
  void click(ButtonId id, int64_t hold_ms = 50, int64_t before_ms = 0) {
    set(id, true, before_ms);
    set(id, false, hold_ms);
  }
  void tick(int64_t advance_ms) {
    now += advance_ms * kMs;
    auto out = fsm.tick(now);
    events.insert(events.end(), out.begin(), out.end());
  }
  int count(ButtonAction a, ButtonId id) const {
    int n = 0;
    for (const auto& e : events) n += e.action == a && e.id == id;
    return n;
  }
  int count(ButtonAction a) const {
    int n = 0;
    for (const auto& e : events) n += e.action == a;
    return n;
  }
};

}  // namespace

TEST(gesture, button_short_press_is_immediate_by_default) {
  for (ButtonId id : {ButtonId::Up, ButtonId::Down, ButtonId::Power}) {
    ButtonRun run;
    CHECK(!run.fsm.doublePressEnabled(id));
    run.click(id);
    REQUIRE(run.events.size() == 3);
    CHECK(run.events[0].action == ButtonAction::Press);
    CHECK(run.events[1].action == ButtonAction::Release);
    CHECK(run.events[2].action == ButtonAction::ShortPress);
    CHECK_EQ(run.events[2].timestamp_us, run.now);  // no 280 ms delay
    // A quick second click is two short presses, never a double.
    run.click(id, 50, 50);
    CHECK_EQ(run.count(ButtonAction::ShortPress), 2);
    CHECK_EQ(run.count(ButtonAction::DoublePress), 0);
  }
}

TEST(gesture, power_never_waits_for_double) {
  ButtonRun run;
  run.fsm.setDoublePressEnabled(ButtonId::Power, true);
  CHECK(!run.fsm.doublePressEnabled(ButtonId::Power));
  run.click(ButtonId::Power);
  CHECK_EQ(run.count(ButtonAction::ShortPress, ButtonId::Power), 1);
  run.click(ButtonId::Power, 50, 50);
  CHECK_EQ(run.count(ButtonAction::ShortPress, ButtonId::Power), 2);
  CHECK_EQ(run.count(ButtonAction::DoublePress), 0);
}

TEST(gesture, button_double_press_opt_in) {
  ButtonRun run;
  run.fsm.setDoublePressEnabled(ButtonId::Down, true);
  CHECK(run.fsm.doublePressEnabled(ButtonId::Down));
  CHECK(!run.fsm.doublePressEnabled(ButtonId::Up));
  run.click(ButtonId::Down);
  CHECK_EQ(run.count(ButtonAction::ShortPress), 0);  // deferred
  run.click(ButtonId::Down, 50, 100);
  CHECK_EQ(run.count(ButtonAction::DoublePress, ButtonId::Down), 1);
  run.tick(1000);
  CHECK_EQ(run.count(ButtonAction::ShortPress), 0);

  // Single click with double enabled -> ShortPress after the 280 ms window.
  ButtonRun single;
  single.fsm.setDoublePressEnabled(ButtonId::Up, true);
  single.click(ButtonId::Up);
  single.tick(270);
  CHECK_EQ(single.count(ButtonAction::ShortPress), 0);
  single.tick(20);
  CHECK_EQ(single.count(ButtonAction::ShortPress, ButtonId::Up), 1);
  // Second click after the window is a fresh press, not a double.
  single.click(ButtonId::Up, 50, 10);
  single.tick(300);
  CHECK_EQ(single.count(ButtonAction::ShortPress, ButtonId::Up), 2);
  CHECK_EQ(single.count(ButtonAction::DoublePress), 0);

  // Disabling again restores immediate short presses.
  single.fsm.setDoublePressEnabled(ButtonId::Up, false);
  single.click(ButtonId::Up, 50, 10);
  CHECK_EQ(single.count(ButtonAction::ShortPress, ButtonId::Up), 3);
}

TEST(gesture, button_long_press) {
  ButtonRun run;
  run.set(ButtonId::Up, true);
  run.tick(690);
  CHECK_EQ(run.count(ButtonAction::LongPress), 0);
  run.tick(10);
  CHECK_EQ(run.count(ButtonAction::LongPress, ButtonId::Up), 1);
  run.tick(1000);
  CHECK_EQ(run.count(ButtonAction::LongPress), 1);
  run.set(ButtonId::Up, false, 10);
  CHECK_EQ(run.count(ButtonAction::ShortPress), 0);
  CHECK_EQ(run.count(ButtonAction::Release), 1);
}

TEST(gesture, button_chord) {
  ButtonRun run;
  run.set(ButtonId::Up, true);
  run.set(ButtonId::Down, true, 100);  // within 140 ms chord window
  CHECK_EQ(run.count(ButtonAction::ChordPress), 1);
  run.tick(1000);
  CHECK_EQ(run.count(ButtonAction::LongPress), 0);  // chord suppresses long press
  run.set(ButtonId::Up, false, 10);
  run.set(ButtonId::Down, false, 10);
  CHECK_EQ(run.count(ButtonAction::ShortPress), 0);

  ButtonRun late;
  late.set(ButtonId::Up, true);
  late.set(ButtonId::Down, true, 200);  // outside window
  CHECK_EQ(late.count(ButtonAction::ChordPress), 0);
}

TEST(gesture, button_wake_release_gate_and_repeated_samples) {
  ButtonGestureFsm fsm;
  // Power held at wake: its first release must not trigger an action.
  fsm.reset(0, 1u << 2, true);
  CHECK(fsm.sample(ButtonId::Power, true, 10 * kMs).empty());
  CHECK(fsm.tick(2000 * kMs).empty());  // no long press while gated
  CHECK(fsm.sample(ButtonId::Power, false, 2100 * kMs).empty());
  auto press = fsm.sample(ButtonId::Power, true, 2200 * kMs);
  REQUIRE(press.size() == 1);
  CHECK(press[0].action == ButtonAction::Press);
  // Repeating the same level (e.g. after debounce) emits nothing.
  CHECK(fsm.sample(ButtonId::Power, true, 2210 * kMs).empty());
  auto release = fsm.sample(ButtonId::Power, false, 2250 * kMs);
  CHECK(release.size() == 2 && release[1].action == ButtonAction::ShortPress);
}
