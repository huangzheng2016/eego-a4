// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// touch_cal_screen —— 设备端五点触摸校准（全屏模态，B/W 快刷）。
//
// 流程：依次显示 5 个十字靶（四角内缩 48px + 中心），每次点按取本次接触全部
// 采样的原始坐标均值（TouchEvent.raw_*，与当前标定无关：靶点在屏幕任意位置
// 点按都接受，即使现有标定严重偏差也能完成）；5 点后最小二乘拟合并校验
// （touch_calibration.h），进入验证：点按任意位置以新标定画出圆点，
// [保存] [重做] [取消]（按新标定命中）。实体键始终可用：Power 短按 = 保存，
// Power 长按 / 前置键 = 取消，↑ 短按 = 重做。90 s 无操作自动取消。
//
// 由 PanelApp 持有：active() 时其 handleTouch/handleButton/service 全部转交本类，
// 结束后 takeOutcome() 取结果并恢复原页面（整页重绘）。

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
  // 开始校准（已在进行中则从第一点重来）。触控不可用时返回 false。
  bool start(int64_t nowMs);
  bool active() const { return active_; }

  void handleTouch(const eego::TouchEvent& ev, int64_t nowMs);
  void handleButton(const eego::ButtonEvent& ev, int64_t nowMs);
  // 超时检查 + 有变化时绘制并 Fast 刷新（app_task）。
  void service(int64_t nowMs);
  // 一次性取走结束结果（None = 仍在进行或未开始）。
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
  bool moved_ = false;     // 本次接触越过 slop（滑动）：不作为采样
  bool rejected_ = false;  // 上一次采样被拒（提示保持不动）
  int64_t lastActivityMs_ = 0;
  struct Dot {
    int16_t x;
    int16_t y;
  };
  Dot dots_[kMaxDots] = {};
  size_t dotCount_ = 0;
};

}  // namespace panel
