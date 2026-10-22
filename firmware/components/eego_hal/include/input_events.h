// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace eego {

inline constexpr int64_t kInputLongPressUs = 700000;
inline constexpr int64_t kInputDoublePressUs = 280000;
inline constexpr int64_t kInputChordWindowUs = 140000;
inline constexpr int64_t kInputActionMaxAgeUs = 750000;
inline constexpr int kTouchSlopPx = 24;
inline constexpr int kTouchSwipeMinPx = 80;
inline constexpr int64_t kTouchSwipeMaxUs = 1200000;

enum class ButtonId : uint8_t { Up = 0, Down, Power };
enum class ButtonAction : uint8_t {
  Press,
  Release,
  ShortPress,
  LongPress,
  DoublePress,
  ChordPress,
};

struct ButtonEvent {
  ButtonId id = ButtonId::Up;
  ButtonAction action = ButtonAction::Press;
  int64_t timestamp_us = 0;
};

enum class TouchEventType : uint8_t {
  None,
  Press,
  Release,
  Move,
  Cancel,
  Tap,
  Hold,
  SwipeLeft,
  SwipeRight,
  FrontPress,
  FrontRelease,
  FrontShort,
  FrontLong,
  FrontDouble,
};

struct TouchEvent {
  TouchEventType type = TouchEventType::None;
  uint16_t x = 0;
  uint16_t y = 0;
  int64_t timestamp_us = 0;
};

}  // namespace eego
