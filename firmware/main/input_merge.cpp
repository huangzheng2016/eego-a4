// SPDX-License-Identifier: GPL-3.0-or-later
#include "input_merge.h"

#include <algorithm>

namespace panel {

void InputEventBatch::clear() {
  events_.clear();
  next_sequence_ = 0;
}

void InputEventBatch::add(const eego::TouchEvent& event) {
  OrderedInputEvent ordered;
  ordered.device = OrderedInputEvent::Device::Touch;
  ordered.sequence = next_sequence_++;
  ordered.touch = event;
  events_.push_back(ordered);
}

void InputEventBatch::add(const eego::ButtonEvent& event) {
  OrderedInputEvent ordered;
  ordered.device = OrderedInputEvent::Device::Button;
  ordered.sequence = next_sequence_++;
  ordered.button = event;
  events_.push_back(ordered);
}

void InputEventBatch::dispatch(PanelApp& app) {
  std::sort(events_.begin(), events_.end(), [](const OrderedInputEvent& left,
                                                const OrderedInputEvent& right) {
    const int64_t left_ts = left.device == OrderedInputEvent::Device::Touch
                                ? left.touch.timestamp_us
                                : left.button.timestamp_us;
    const int64_t right_ts = right.device == OrderedInputEvent::Device::Touch
                                 ? right.touch.timestamp_us
                                 : right.button.timestamp_us;
    if (left_ts != right_ts) return left_ts < right_ts;
    if (left.device != right.device) return left.device == OrderedInputEvent::Device::Button;
    return left.sequence < right.sequence;
  });
  for (const auto& event : events_) {
    if (event.device == OrderedInputEvent::Device::Touch) {
      app.handleTouch(event.touch);
    } else {
      app.handleButton(event.button);
    }
  }
}

}  // namespace panel
