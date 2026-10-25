// SPDX-License-Identifier: GPL-3.0-or-later
// PanelApp 触摸校准 / 触点诊断（app_task 上下文）：
//   * 设置「触摸校准」与串口 `touch cal` → 全屏五点校准（touch_cal_screen）；
//   * 串口 `touch [trace]` → 20 s 内逐事件打印原始坐标与映射坐标；
//   * 设置「显示触点」与串口 `touch marks on|off` → 下一次刷新在点按处画标记；
//   * 串口 `touch show|reset` → 打印当前标定 / 清除标定恢复出厂映射。
#include <cstdio>
#include <cstring>
#include <strings.h>  // strcasecmp

#include "esp_log.h"
#include "esp_timer.h"
#include "panel_app.h"

namespace panel {

namespace {

constexpr const char* TAG = "panel_touch";
constexpr int64_t kTouchTraceMs = 20000;

const char* touchTypeName(eego::TouchEventType type) {
  switch (type) {
    case eego::TouchEventType::None: return "none";
    case eego::TouchEventType::Press: return "press";
    case eego::TouchEventType::Release: return "release";
    case eego::TouchEventType::Move: return "move";
    case eego::TouchEventType::Cancel: return "cancel";
    case eego::TouchEventType::Tap: return "tap";
    case eego::TouchEventType::Hold: return "hold";
    case eego::TouchEventType::SwipeLeft: return "swipe-left";
    case eego::TouchEventType::SwipeRight: return "swipe-right";
    case eego::TouchEventType::SwipeUp: return "swipe-up";
    case eego::TouchEventType::SwipeDown: return "swipe-down";
    case eego::TouchEventType::FrontPress: return "front-press";
    case eego::TouchEventType::FrontRelease: return "front-release";
    case eego::TouchEventType::FrontShort: return "front-short";
    case eego::TouchEventType::FrontLong: return "front-long";
    case eego::TouchEventType::FrontDouble: return "front-double";
  }
  return "?";
}

void printAffine(const char* prefix, const eego::touchcal::Affine& t) {
  printf("%s a=%ld b=%ld c=%ld d=%ld e=%ld f=%ld den=%ld\n", prefix, static_cast<long>(t.a),
         static_cast<long>(t.b), static_cast<long>(t.c), static_cast<long>(t.d),
         static_cast<long>(t.e), static_cast<long>(t.f), static_cast<long>(t.den));
}

}  // namespace

void PanelApp::startTouchCalibration() {
  if (deps_.touch == nullptr || !touchCal_.start(nowMs())) {
    showBanner(tr(Str::Unavailable), 3000);
    return;
  }
  if (confirm_ != ConfirmKind::None) clearConfirm();
  touchMarkPending_ = false;
}

bool PanelApp::serviceTouchCalibration(int64_t now) {
  if (touchCal_.active()) {
    touchCal_.service(now);
    if (touchCal_.active()) {
      noteInputResume();  // 校准屏刷新会阻塞 app_task
      return true;
    }
  }
  const TouchCalScreen::Outcome outcome = touchCal_.takeOutcome();
  if (outcome == TouchCalScreen::Outcome::None) return false;
  // 恢复原页面：整页重绘（校准屏覆盖了整块面板）。
  if (page_ == Page::Dashboard) dashboard_.invalidateAll();
  markDirty(true);
  noteInteraction(now);
  if (outcome == TouchCalScreen::Outcome::Saved) {
    showBanner(tr(Str::TouchCalSaved), 4000);
  } else if (outcome == TouchCalScreen::Outcome::SaveFailed) {
    showBanner(tr(Str::ActionFailed), 5000);
  }
  return false;
}

void PanelApp::traceTouch(const eego::TouchEvent& ev) {
  if (touchTraceUntilMs_ == 0) return;
  const int64_t now = esp_timer_get_time() / 1000;
  if (now > touchTraceUntilMs_) {
    touchTraceUntilMs_ = 0;
    printf("touch: trace finished\n");
    return;
  }
  if (ev.raw_samples == 0) {
    printf("touch %-13s mapped=(%u,%u)\n", touchTypeName(ev.type), ev.x, ev.y);
  } else {
    printf("touch %-13s raw=(%u,%u) n=%u mapped=(%u,%u)\n", touchTypeName(ev.type), ev.raw_x,
           ev.raw_y, ev.raw_samples, ev.x, ev.y);
  }
}

void PanelApp::noteTouchMark(int16_t x, int16_t y) {
  touchMarkX_ = x;
  touchMarkY_ = y;
  touchMarkPending_ = true;
  if (page_ != Page::Dashboard) markDirty();  // 页面整页重画时叠加；dashboard 见 service
}

void PanelApp::drawTouchMark(eego::PortraitCanvas& c) const {
  if (!touchMarks_ || touchMarkX_ < 0 || touchMarkY_ < 0) return;
  c.resetClip();
  const int16_t x = touchMarkX_;
  const int16_t y = touchMarkY_;
  // 白边黑心圆点 + 十字：在任何背景上都可见。
  c.fillCircle(x, y, 9, 1);
  c.fillCircle(x, y, 6, 0);
  c.drawCircle(x, y, 10, 0);
  c.drawLine(static_cast<int16_t>(x - 16), y, static_cast<int16_t>(x - 11), y, 0);
  c.drawLine(static_cast<int16_t>(x + 11), y, static_cast<int16_t>(x + 16), y, 0);
  c.drawLine(x, static_cast<int16_t>(y - 16), x, static_cast<int16_t>(y - 11), 0);
  c.drawLine(x, static_cast<int16_t>(y + 11), x, static_cast<int16_t>(y + 16), 0);
}

void PanelApp::diagTouch(const char* arg) {
  const char* a = arg != nullptr ? arg : "";
  if (*a == '\0' || strcasecmp(a, "trace") == 0) {
    touchTraceUntilMs_ = esp_timer_get_time() / 1000 + kTouchTraceMs;
    printf("touch: tracing raw/mapped coordinates for %lds (tap the screen)\n",
           static_cast<long>(kTouchTraceMs / 1000));
    if (deps_.touch != nullptr) printAffine("touch: calibration", deps_.touch->calibration());
    return;
  }
  if (strcasecmp(a, "cal") == 0) {
    startTouchCalibration();
    printf(touchCal_.active() ? "touch: calibration screen started\n"
                              : "ERR touch cal: touch unavailable\n");
    return;
  }
  if (strcasecmp(a, "marks on") == 0 || strcasecmp(a, "marks off") == 0 ||
      strcasecmp(a, "marks") == 0) {
    touchMarks_ = strcasecmp(a, "marks off") != 0;
    touchMarkX_ = touchMarkY_ = -1;
    touchMarkPending_ = false;
    if (page_ == Page::Settings) markDirty();
    printf("touch: marks %s\n", touchMarks_ ? "on" : "off");
    return;
  }
  if (deps_.touch == nullptr) {
    printf("ERR touch: unavailable\n");
    return;
  }
  if (strcasecmp(a, "show") == 0) {
    printf("touch: %s\n", deps_.touch->calibrationCustom() ? "calibrated" : "factory mapping");
    printAffine("touch: calibration", deps_.touch->calibration());
    return;
  }
  if (strcasecmp(a, "reset") == 0) {
    const esp_err_t err = deps_.touch->clearCalibration();
    ESP_LOGI(TAG, "touch calibration cleared (%s)", esp_err_to_name(err));
    printf("touch: calibration reset to factory mapping (%s)\n", esp_err_to_name(err));
    if (page_ == Page::Settings) markDirty();
    return;
  }
  printf("ERR usage: touch [trace]|cal|marks on|off|show|reset\n");
}

}  // namespace panel
