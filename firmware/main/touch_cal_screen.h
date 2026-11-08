// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

//
//

#include <cstddef>
#include <cstdint>

#include "eego_hal.h"
#include "eego_ui.h"
#include "touch_calibration.h"

namespace panel {
class TouchCalScreen {
 public:
  enum class Outcome : uint8_t { None, Saved, SaveFailed, Cancelled };

  void begin(eego::PortraitCanvas* canvas, eego::Gslx680Touch* touch);
  bool start(int64_t nowMs);
  bool active() const { return active_; }

  void handleTouch(const eego::TouchEvent& ev, int64_t nowMs);
  void handleButton(const eego::ButtonEvent& ev, int64_t nowMs);
  void service(int64_t nowMs);
  Outcome takeOutcome();

 private:
  enum class Phase : uint8_t { Collect, Verify, Failed };
  enum class Button : uint8_t { None, Save, Redo, Cancel };
  static constexpr size_t kMaxDots = 12;

  void restart(int64_t nowMs);
  void finish(Outcome outcome);
  void save();
  void collect(const eego::TouchEvent& ev);
  void verifyTap(const eego::TouchEvent& ev);
  Button buttonAt(int16_t x, int16_t y) const;
  void render();
  void drawCross(int16_t x, int16_t y, int16_t arm, bool heavy);
  void drawButtons();

  eego::PortraitCanvas* canvas_ = nullptr;
  eego::Gslx680Touch* touch_ = nullptr;
  bool active_ = false;
  bool dirty_ = false;
  Phase phase_ = Phase::Collect;
  Outcome outcome_ = Outcome::None;
  eego::touchcal::Session session_;
  eego::touchcal::FitResult fit_;
  bool moved_ = false;
  bool rejected_ = false;
  int64_t lastActivityMs_ = 0;
  struct Dot {
    int16_t x;
    int16_t y;
  };
  Dot dots_[kMaxDots] = {};
  size_t dotCount_ = 0;
};

}  // namespace panel
