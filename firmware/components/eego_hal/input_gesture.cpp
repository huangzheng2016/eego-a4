// SPDX-License-Identifier: GPL-3.0-or-later
#include "input_gesture.h"

#include <cstdlib>

namespace eego {

namespace {

int absolute(int value) { return value < 0 ? -value : value; }

}  // namespace

size_t ButtonGestureFsm::index(ButtonId id) {
  switch (id) {
    case ButtonId::Up: return 0;
    case ButtonId::Down: return 1;
    case ButtonId::Power: return 2;
  }
  return 0;
}

void ButtonGestureFsm::reset(int64_t now_us, uint8_t pressed_mask,
                             bool wake_release_gate) {
  for (size_t i = 0; i < 3; ++i) {
    states_[i] = State{};
    if (wake_release_gate && (pressed_mask & (1u << i)) != 0) {
      states_[i].down = true;
      states_[i].gated = true;
      states_[i].down_since_us = now_us;
    }
  }
}

std::vector<ButtonEvent> ButtonGestureFsm::tick(int64_t now_us) {
  std::vector<ButtonEvent> out;
  for (size_t i = 0; i < 3; ++i) {
    State& current = states_[i];
    const ButtonId id = static_cast<ButtonId>(i);
    if (current.down && !current.gated && !current.chord && !current.long_sent &&
        now_us - current.down_since_us >= kInputLongPressUs) {
      current.long_sent = true;
      current.double_armed = false;
      current.pending_short_deadline_us = 0;
      out.push_back({id, ButtonAction::LongPress, now_us});
    }
    if (!current.down && current.pending_short_deadline_us != 0 &&
        now_us >= current.pending_short_deadline_us) {
      current.pending_short_deadline_us = 0;
      out.push_back({id, ButtonAction::ShortPress, now_us});
    }
  }
  return out;
}

std::vector<ButtonEvent> ButtonGestureFsm::sample(ButtonId id, bool pressed,
                                                   int64_t now_us) {
  std::vector<ButtonEvent> out = tick(now_us);
  State& current = state(id);
  if (current.gated) {
    if (!pressed) {
      current = State{};
    }
    return out;
  }
  if (pressed == current.down) return out;

  if (pressed) {
    current.down = true;
    current.down_since_us = now_us;
    current.long_sent = false;
    out.push_back({id, ButtonAction::Press, now_us});

    if (isUpDown(id)) {
      State& other = state(otherUpDown(id));
      if (other.down && !other.gated && !other.chord && !other.long_sent &&
          now_us - other.down_since_us <= kInputChordWindowUs) {
        current.chord = true;
        other.chord = true;
        current.double_armed = false;
        other.double_armed = false;
        current.pending_short_deadline_us = 0;
        other.pending_short_deadline_us = 0;
        out.push_back({ButtonId::Up, ButtonAction::ChordPress, now_us});
        return out;
      }
    }

    if (current.pending_short_deadline_us != 0 &&
        now_us <= current.pending_short_deadline_us) {
      current.double_armed = true;
      current.pending_short_deadline_us = 0;
    }
    return out;
  }

  current.down = false;
  out.push_back({id, ButtonAction::Release, now_us});
  if (current.chord) {
    current.chord = false;
    current.double_armed = false;
    current.pending_short_deadline_us = 0;
    return out;
  }
  if (current.long_sent) {
    current.double_armed = false;
    current.pending_short_deadline_us = 0;
    return out;
  }
  if (current.double_armed) {
    current.double_armed = false;
    current.pending_short_deadline_us = 0;
    out.push_back({id, ButtonAction::DoublePress, now_us});
    return out;
  }
  current.pending_short_deadline_us = now_us + kInputDoublePressUs;
  return out;
}

void TouchGestureFsm::reset() {
  active_ = TouchContact::None;
  start_x_ = start_y_ = last_x_ = last_y_ = 0;
  down_since_us_ = 0;
  pending_front_short_deadline_us_ = 0;
  moved_ = false;
  long_sent_ = false;
  front_double_armed_ = false;
}

void TouchGestureFsm::cancel(int64_t now_us, std::vector<TouchEvent>& out) {
  if (active_ == TouchContact::None) return;
  out.push_back({TouchEventType::Cancel, last_x_, last_y_, now_us});
  active_ = TouchContact::None;
  moved_ = false;
  long_sent_ = false;
  front_double_armed_ = false;
  pending_front_short_deadline_us_ = 0;
}

void TouchGestureFsm::start(TouchContact contact, uint16_t x, uint16_t y,
                            int64_t now_us, std::vector<TouchEvent>& out) {
  active_ = contact;
  start_x_ = last_x_ = x;
  start_y_ = last_y_ = y;
  down_since_us_ = now_us;
  moved_ = false;
  long_sent_ = false;
  if (contact == TouchContact::Screen) {
    pending_front_short_deadline_us_ = 0;
    front_double_armed_ = false;
    out.push_back({TouchEventType::Press, x, y, now_us});
    return;
  }
  if (pending_front_short_deadline_us_ != 0 &&
      now_us <= pending_front_short_deadline_us_) {
    front_double_armed_ = true;
    pending_front_short_deadline_us_ = 0;
  }
  out.push_back({TouchEventType::FrontPress, 0, 0, now_us});
}

void TouchGestureFsm::release(int64_t now_us, std::vector<TouchEvent>& out) {
  if (active_ == TouchContact::Screen) {
    out.push_back({TouchEventType::Release, last_x_, last_y_, now_us});
    if (!long_sent_) {
      const int dx = static_cast<int>(last_x_) - static_cast<int>(start_x_);
      const int dy = static_cast<int>(last_y_) - static_cast<int>(start_y_);
      const int ax = absolute(dx);
      const int ay = absolute(dy);
      if (ax >= kTouchSwipeMinPx && ax >= ay * 2 &&
          now_us - down_since_us_ <= kTouchSwipeMaxUs) {
        out.push_back({dx < 0 ? TouchEventType::SwipeLeft : TouchEventType::SwipeRight,
                       last_x_, last_y_, now_us});
      } else if (!moved_) {
        out.push_back({TouchEventType::Tap, last_x_, last_y_, now_us});
      }
    }
  } else if (active_ == TouchContact::Front) {
    out.push_back({TouchEventType::FrontRelease, 0, 0, now_us});
    if (!long_sent_) {
      if (front_double_armed_) {
        out.push_back({TouchEventType::FrontDouble, 0, 0, now_us});
      } else {
        pending_front_short_deadline_us_ = now_us + kInputDoublePressUs;
      }
    }
  }
  active_ = TouchContact::None;
  moved_ = false;
  long_sent_ = false;
  front_double_armed_ = false;
}

std::vector<TouchEvent> TouchGestureFsm::invalidate(int64_t now_us) {
  const bool had_activity = active_ != TouchContact::None || pending_front_short_deadline_us_ != 0;
  const uint16_t x = last_x_;
  const uint16_t y = last_y_;
  reset();
  std::vector<TouchEvent> out;
  if (had_activity) out.push_back({TouchEventType::Cancel, x, y, now_us});
  return out;
}

std::vector<TouchEvent> TouchGestureFsm::tick(int64_t now_us) {
  std::vector<TouchEvent> out;
  if (active_ != TouchContact::None && !moved_ && !long_sent_ &&
      now_us - down_since_us_ >= kInputLongPressUs) {
    long_sent_ = true;
    pending_front_short_deadline_us_ = 0;
    front_double_armed_ = false;
    out.push_back({active_ == TouchContact::Front ? TouchEventType::FrontLong
                                                  : TouchEventType::Hold,
                   last_x_, last_y_, now_us});
  }
  if (active_ == TouchContact::None && pending_front_short_deadline_us_ != 0 &&
      now_us >= pending_front_short_deadline_us_) {
    pending_front_short_deadline_us_ = 0;
    out.push_back({TouchEventType::FrontShort, 0, 0, now_us});
  }
  return out;
}

std::vector<TouchEvent> TouchGestureFsm::sample(TouchContact contact, uint16_t x,
                                                 uint16_t y, int64_t now_us) {
  std::vector<TouchEvent> out = tick(now_us);
  if (contact != TouchContact::None && active_ != TouchContact::None &&
      contact != active_) {
    cancel(now_us, out);
  }
  if (contact == TouchContact::None) {
    release(now_us, out);
    return out;
  }
  if (active_ == TouchContact::None) {
    start(contact, x, y, now_us, out);
    return out;
  }
  if (contact == TouchContact::Front) return out;

  if (x != last_x_ || y != last_y_) {
    last_x_ = x;
    last_y_ = y;
    if (absolute(static_cast<int>(x) - static_cast<int>(start_x_)) > kTouchSlopPx ||
        absolute(static_cast<int>(y) - static_cast<int>(start_y_)) > kTouchSlopPx) {
      moved_ = true;
    }
    out.push_back({TouchEventType::Move, x, y, now_us});
  }
  return out;
}

}  // namespace eego
