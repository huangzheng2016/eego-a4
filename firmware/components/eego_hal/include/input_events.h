// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace eego {

inline constexpr int64_t kInputLongPressUs = 700000;
inline constexpr int64_t kInputDoublePressUs = 280000;
inline constexpr int64_t kInputChordWindowUs = 140000;
inline constexpr int64_t kInputActionMaxAgeUs = 750000;
inline constexpr int kTouchSlopPx = 24;
inline constexpr int kTouchSwipeMinPx = 60;
// 主方向位移须 >= 次方向位移 × 1.5（以整数 2*major >= 3*minor 判定）。
inline constexpr int kTouchSwipeRatioNum = 3;
inline constexpr int kTouchSwipeRatioDen = 2;
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
  SwipeUp,    // 手指向上划（y 减小）
  SwipeDown,  // 手指向下划（y 增大）
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
  // 触控原始坐标（GSLX680 12 位字，标定/诊断用；前置键与手势 FSM 单测中为 0）。
  // Press/Move：本帧原始值；Release/Tap/Hold/Swipe*/Cancel：本次接触全部采样的均值。
  uint16_t raw_x = 0;
  uint16_t raw_y = 0;
  uint16_t raw_samples = 0;  // 参与均值的采样帧数（0 = 无原始数据）
};

}  // namespace eego
