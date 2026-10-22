// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <vector>

#include "input_events.h"

namespace eego {

class ButtonGestureFsm {
 public:
  void reset(int64_t now_us, uint8_t pressed_mask, bool wake_release_gate);
  std::vector<ButtonEvent> sample(ButtonId id, bool pressed, int64_t now_us);
  std::vector<ButtonEvent> tick(int64_t now_us);

 private:
  struct State {
    bool down = false;
    bool gated = false;
    bool chord = false;
    bool long_sent = false;
    bool double_armed = false;
    int64_t down_since_us = 0;
    int64_t pending_short_deadline_us = 0;
  };

  static size_t index(ButtonId id);
  State& state(ButtonId id) { return states_[index(id)]; }
  const State& state(ButtonId id) const { return states_[index(id)]; }
  bool isUpDown(ButtonId id) const { return id != ButtonId::Power; }
  ButtonId otherUpDown(ButtonId id) const {
    return id == ButtonId::Up ? ButtonId::Down : ButtonId::Up;
  }

  State states_[3]{};
};

enum class TouchContact : uint8_t { None, Screen, Front };

class TouchGestureFsm {
 public:
  void reset();
  std::vector<TouchEvent> sample(TouchContact contact, uint16_t x, uint16_t y,
                                 int64_t now_us);
  std::vector<TouchEvent> invalidate(int64_t now_us);
  std::vector<TouchEvent> tick(int64_t now_us);
  bool screenPressed() const { return active_ == TouchContact::Screen; }

 private:
  void start(TouchContact contact, uint16_t x, uint16_t y, int64_t now_us,
             std::vector<TouchEvent>& out);
  void release(int64_t now_us, std::vector<TouchEvent>& out);
  void cancel(int64_t now_us, std::vector<TouchEvent>& out);

  TouchContact active_ = TouchContact::None;
  uint16_t start_x_ = 0;
  uint16_t start_y_ = 0;
  uint16_t last_x_ = 0;
  uint16_t last_y_ = 0;
  int64_t down_since_us_ = 0;
  int64_t pending_front_short_deadline_us_ = 0;
  bool moved_ = false;
  bool long_sent_ = false;
  bool front_double_armed_ = false;
};

}  // namespace eego
