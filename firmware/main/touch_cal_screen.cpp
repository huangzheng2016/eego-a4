// SPDX-License-Identifier: GPL-3.0-or-later
// 五点触摸校准屏（见 touch_cal_screen.h）。
#include "touch_cal_screen.h"

#include <cmath>
#include <cstdio>

#include "esp_log.h"
#include "panel_render.h"
#include "ui_text.h"

namespace panel {

namespace {

constexpr const char* TAG = "touch_cal";
constexpr int64_t kIdleCancelMs = 90000;

// 验证页按钮行（全宽三等分，位于中心靶与底部靶之间）。
constexpr int16_t kBtnY = 588;
constexpr int16_t kBtnH = 64;
constexpr int16_t kBtnGap = 12;
constexpr int16_t kBtnX = 28;
constexpr int16_t kBtnW = (496 - 2 * kBtnGap) / 3;

constexpr int16_t kTextX = 40;
constexpr int16_t kTextW = 472;

}  // namespace

void TouchCalScreen::begin(eego::PortraitCanvas* canvas, eego::Gslx680Touch* touch) {
  canvas_ = canvas;
  touch_ = touch;
}

bool TouchCalScreen::start(int64_t nowMs) {
  if (canvas_ == nullptr || touch_ == nullptr) return false;
  active_ = true;
  outcome_ = Outcome::None;
  restart(nowMs);
  ESP_LOGI(TAG, "touch calibration started");
  return true;
}

void TouchCalScreen::restart(int64_t nowMs) {
  session_.reset();
  fit_ = eego::touchcal::FitResult{};
  phase_ = Phase::Collect;
  dotCount_ = 0;
  moved_ = false;
  rejected_ = false;
  lastActivityMs_ = nowMs;
  dirty_ = true;
}

void TouchCalScreen::finish(Outcome outcome) {
  active_ = false;
  dirty_ = false;
  outcome_ = outcome;
  ESP_LOGI(TAG, "touch calibration finished: %d", static_cast<int>(outcome));
}

TouchCalScreen::Outcome TouchCalScreen::takeOutcome() {
  const Outcome out = outcome_;
  outcome_ = Outcome::None;
  return out;
}

void TouchCalScreen::save() {
  if (phase_ != Phase::Verify || !fit_.ok) return;
  const esp_err_t err = touch_->saveCalibration(fit_.affine);
  const eego::touchcal::Affine& t = fit_.affine;
  ESP_LOGI(TAG, "save %s: a=%ld b=%ld c=%ld d=%ld e=%ld f=%ld den=%ld max=%.1fpx",
           err == ESP_OK ? "ok" : "FAILED", static_cast<long>(t.a), static_cast<long>(t.b),
           static_cast<long>(t.c), static_cast<long>(t.d), static_cast<long>(t.e),
           static_cast<long>(t.f), static_cast<long>(t.den), fit_.maxResidualPx);
  finish(err == ESP_OK ? Outcome::Saved : Outcome::SaveFailed);
}

void TouchCalScreen::handleButton(const eego::ButtonEvent& ev, int64_t nowMs) {
  if (!active_) return;
  lastActivityMs_ = nowMs;
  if (ev.id == eego::ButtonId::Power) {
    if (ev.action == eego::ButtonAction::LongPress) {
      finish(Outcome::Cancelled);
    } else if (ev.action == eego::ButtonAction::ShortPress) {
      if (phase_ == Phase::Verify) save();
      else if (phase_ == Phase::Failed) restart(nowMs);
    }
    return;
  }
  if (ev.id == eego::ButtonId::Up && ev.action == eego::ButtonAction::ShortPress) restart(nowMs);
}

void TouchCalScreen::handleTouch(const eego::TouchEvent& ev, int64_t nowMs) {
  if (!active_) return;
  lastActivityMs_ = nowMs;
  switch (ev.type) {
    case eego::TouchEventType::FrontShort:
    case eego::TouchEventType::FrontLong:
      finish(Outcome::Cancelled);
      return;
    case eego::TouchEventType::Press:
      moved_ = false;
      return;
    case eego::TouchEventType::Move:
      moved_ = true;
      return;
    case eego::TouchEventType::Release:
      break;
    default:
      return;  // Tap/Hold/Swipe 与 Release 同属一次接触，只按 Release 处理
  }
  if (ev.raw_samples == 0) return;
  switch (phase_) {
    case Phase::Collect:
      collect(ev);
      break;
    case Phase::Verify:
      verifyTap(ev);
      break;
    case Phase::Failed:
      restart(nowMs);  // 任意位置点按重做（不依赖任何标定）
      break;
  }
}

void TouchCalScreen::collect(const eego::TouchEvent& ev) {
  if (moved_ || !session_.add(ev.raw_x, ev.raw_y, ev.raw_samples)) {
    rejected_ = true;
    dirty_ = true;
    return;
  }
  ESP_LOGI(TAG, "point %u raw=(%u,%u) n=%u", static_cast<unsigned>(session_.step()),
           ev.raw_x, ev.raw_y, ev.raw_samples);
  rejected_ = false;
  dirty_ = true;
  if (!session_.complete()) return;
  fit_ = session_.solve();
  ESP_LOGI(TAG, "fit %s max=%.1fpx rms=%.1fpx", eego::touchcal::fitErrorName(fit_.error),
           fit_.maxResidualPx, fit_.rmsResidualPx);
  phase_ = fit_.ok ? Phase::Verify : Phase::Failed;
}

TouchCalScreen::Button TouchCalScreen::buttonAt(int16_t x, int16_t y) const {
  if (y < kBtnY || y >= kBtnY + kBtnH || x < kBtnX) return Button::None;
  const int idx = (x - kBtnX) / (kBtnW + kBtnGap);
  if (idx > 2 || x - kBtnX - idx * (kBtnW + kBtnGap) >= kBtnW) return Button::None;
  return idx == 0 ? Button::Save : idx == 1 ? Button::Redo : Button::Cancel;
}

void TouchCalScreen::verifyTap(const eego::TouchEvent& ev) {
  uint16_t x = 0;
  uint16_t y = 0;
  eego::touchcal::map(fit_.affine, ev.raw_x, ev.raw_y, x, y);
  const int16_t sx = static_cast<int16_t>(x);
  const int16_t sy = static_cast<int16_t>(y);
  if (!moved_) {
    switch (buttonAt(sx, sy)) {
      case Button::Save:
        save();
        return;
      case Button::Redo:
        restart(lastActivityMs_);
        return;
      case Button::Cancel:
        finish(Outcome::Cancelled);
        return;
      case Button::None:
        break;
    }
  }
  if (dotCount_ == kMaxDots) {
    for (size_t i = 1; i < kMaxDots; ++i) dots_[i - 1] = dots_[i];
    --dotCount_;
  }
  dots_[dotCount_++] = {sx, sy};
  dirty_ = true;
}

void TouchCalScreen::service(int64_t nowMs) {
  if (!active_) return;
  if (nowMs - lastActivityMs_ > kIdleCancelMs) {
    finish(Outcome::Cancelled);
    return;
  }
  if (dirty_) render();
}

void TouchCalScreen::drawCross(int16_t x, int16_t y, int16_t arm, bool heavy) {
  eego::PortraitCanvas& c = *canvas_;
  const int16_t w = heavy ? 1 : 0;  // 粗线：±1 像素三线
  for (int16_t o = -w; o <= w; ++o) {
    c.drawLine(static_cast<int16_t>(x - arm), static_cast<int16_t>(y + o),
               static_cast<int16_t>(x + arm), static_cast<int16_t>(y + o), 0);
    c.drawLine(static_cast<int16_t>(x + o), static_cast<int16_t>(y - arm),
               static_cast<int16_t>(x + o), static_cast<int16_t>(y + arm), 0);
  }
  if (heavy) {
    c.drawCircle(x, y, static_cast<int16_t>(arm / 2), 0);
    c.drawCircle(x, y, static_cast<int16_t>(arm / 2 + 1), 0);
  }
}

void TouchCalScreen::drawButtons() {
  eego::PortraitCanvas& c = *canvas_;
  const char* labels[3] = {tr(Str::TouchCalSave), tr(Str::TouchCalRedo),
                           tr(Str::TouchCalCancel)};
  const int16_t th = static_cast<int16_t>(eego::FontManager::instance().uiFontHeight() * 2);
  for (int i = 0; i < 3; ++i) {
    const int16_t x = static_cast<int16_t>(kBtnX + i * (kBtnW + kBtnGap));
    if (i == 0) {
      c.fillRoundRect(x, kBtnY, kBtnW, kBtnH, 8, 0);
    } else {
      c.drawRoundRect(x, kBtnY, kBtnW, kBtnH, 8, 0);
      c.drawRoundRect(static_cast<int16_t>(x + 1), static_cast<int16_t>(kBtnY + 1),
                      static_cast<int16_t>(kBtnW - 2), static_cast<int16_t>(kBtnH - 2), 7, 0);
    }
    const int16_t ty = static_cast<int16_t>(kBtnY + (kBtnH - th) / 2);
    if (i == 0) {
      // 黑底白字：逐像素反色不可用，改为白色文字（FontManager 灰度 15）
      const int16_t tw = static_cast<int16_t>(eego::FontManager::instance().uiTextWidth(labels[i]) * 2);
      eego::FontManager::instance().drawTextGray(c, static_cast<int16_t>(x + (kBtnW - tw) / 2), ty,
                                                 labels[i], 2, 15);
    } else {
      c.drawTextCentered(x, ty, kBtnW, labels[i], 2, true);
    }
  }
}

void TouchCalScreen::render() {
  dirty_ = false;
  eego::PortraitCanvas& c = *canvas_;
  c.resetClip();
  c.clear();
  const int16_t lh2 = lineHeight(2);
  const int16_t lh1 = lineHeight(1);
  char buf[96];

  switch (phase_) {
    case Phase::Collect: {
      const size_t step = session_.step();
      c.drawTextCentered(kTextX, 168, kTextW, tr(Str::SetTouchCal), 2, true);
      const eego::touchcal::Target t = eego::touchcal::target(step);
      drawCross(t.x, t.y, 24, true);
      snprintf(buf, sizeof(buf), tr(Str::TouchCalTapFmt), static_cast<unsigned>(step + 1),
               static_cast<unsigned>(eego::touchcal::kTargetCount));
      c.drawTextCentered(kTextX, 468, kTextW, buf, 2, false);
      if (rejected_) {
        c.drawTextCentered(kTextX, static_cast<int16_t>(468 + lh2 + 8), kTextW,
                           tr(Str::TouchCalHoldStill), 1, true);
      }
      c.drawTextCentered(kTextX, static_cast<int16_t>(468 + lh2 + 8 + lh1 + 6), kTextW,
                         tr(Str::TouchCalCancelHint), 1, false);
      break;
    }
    case Phase::Verify: {
      for (size_t i = 0; i < eego::touchcal::kTargetCount; ++i) {
        const eego::touchcal::Target t = eego::touchcal::target(i);
        drawCross(t.x, t.y, 12, false);
      }
      c.drawTextCentered(kTextX, 112, kTextW, tr(Str::SetTouchCal), 2, true);
      drawTextWrapped(c, kTextX, static_cast<int16_t>(112 + lh2 + 8), kTextW,
                      tr(Str::TouchCalVerify), 2, lh2, 2);
      snprintf(buf, sizeof(buf), tr(Str::TouchCalErrorFmt),
               static_cast<unsigned>(std::lround(fit_.maxResidualPx)));
      c.drawTextCentered(kTextX, static_cast<int16_t>(112 + 3 * lh2 + 16), kTextW, buf, 1, false);
      drawButtons();
      c.drawTextCentered(kTextX, static_cast<int16_t>(kBtnY + kBtnH + 10), kTextW,
                         tr(Str::TouchCalKeys), 1, false);
      for (size_t i = 0; i < dotCount_; ++i) {
        c.fillCircle(dots_[i].x, dots_[i].y, 7, 0);
        c.drawCircle(dots_[i].x, dots_[i].y, 9, 0);
      }
      break;
    }
    case Phase::Failed: {
      c.drawTextCentered(kTextX, 168, kTextW, tr(Str::SetTouchCal), 2, true);
      drawTextWrapped(c, kTextX, 300, kTextW, tr(Str::TouchCalFailed), 2, lh2, 3);
      if (fit_.maxResidualPx > 0) {
        snprintf(buf, sizeof(buf), tr(Str::TouchCalErrorFmt),
                 static_cast<unsigned>(std::lround(fit_.maxResidualPx)));
      } else {
        snprintf(buf, sizeof(buf), "%s", eego::touchcal::fitErrorName(fit_.error));
      }
      c.drawTextCentered(kTextX, static_cast<int16_t>(300 + 3 * lh2 + 12), kTextW, buf, 1, false);
      break;
    }
  }
  c.display(eego::RefreshMode::Fast);
  c.waitRefreshComplete();
}

}  // namespace panel
