// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "input_events.h"
#include "panel_app.h"

namespace panel {

struct OrderedInputEvent {
  enum class Device : uint8_t { Touch, Button };

  Device device = Device::Touch;
  uint32_t sequence = 0;
  eego::TouchEvent touch;
  eego::ButtonEvent button;
};

// One app_task batch: all events currently observable are ordered before any
// handler can execute a dangerous action. Buttons win equal timestamps so a
// cross-device cancellation fails closed; per-device FIFO is retained by sequence.
class InputEventBatch {
 public:
  void clear();
  void add(const eego::TouchEvent& event);
  void add(const eego::ButtonEvent& event);
  void dispatch(PanelApp& app);
  size_t size() const { return events_.size(); }
  const OrderedInputEvent& operator[](size_t index) const { return events_[index]; }

 private:
  std::vector<OrderedInputEvent> events_;
  uint32_t next_sequence_ = 0;
};

}  // namespace panel
