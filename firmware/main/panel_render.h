// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

//
//
//

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "device_status.h"
#include "eego_ui.h"
#include "ui_text.h"

namespace panel {
struct TextStyle {
  int16_t scale;
  bool bold;
  uint8_t gray;
};

namespace style {
constexpr TextStyle kTitle{2, true, 0};
constexpr TextStyle kBody{2, false, 0};
constexpr TextStyle kBodyStrong{2, true, 0};
constexpr TextStyle kBodyMuted{2, false, 6};
constexpr TextStyle kCaption{1, false, 6};
constexpr TextStyle kCaptionStrong{1, true, 0};
constexpr TextStyle kSmall{1, false, 0};
constexpr TextStyle kHeadline{3, true, 0};
}  // namespace style

namespace geom {
struct Rect {
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
  constexpr bool contains(int16_t px, int16_t py) const {
    return px >= x && py >= y && px < x + w && py < y + h;
  }
  constexpr int16_t right() const { return static_cast<int16_t>(x + w); }
  constexpr int16_t bottom() const { return static_cast<int16_t>(y + h); }
};

constexpr int16_t kContentX = 28;
constexpr int16_t kContentY = 28;
constexpr int16_t kContentW = 496;
constexpr int16_t kStatusBarH = 44;
constexpr int16_t kSoftKeyBarH = 68;
constexpr int16_t kContentTop = 28 + kStatusBarH + 4;                    // 76
constexpr int16_t kContentBottom = 28 + 712 - kSoftKeyBarH - 4;          // 668
constexpr int16_t kSoftKeyY = 28 + 712 - kSoftKeyBarH;                   // 672
constexpr int16_t kSoftKeyH = 60;
constexpr int16_t kSoftKeyGap = 8;

constexpr int16_t kPadX = 12;
constexpr int16_t kRowPadX = 16;
constexpr int16_t kInnerX = kContentX + kPadX;                           // 40
constexpr int16_t kInnerW = kContentW - 2 * kPadX;                       // 472
constexpr int16_t kInnerRight = kInnerX + kInnerW;                       // 512

constexpr int16_t kBannerH = 28;
constexpr int16_t kTitleY = kContentTop + kBannerH + 4;                  // 108
constexpr int16_t kTitleH = 40;
constexpr int16_t kBodyTop = kTitleY + kTitleH + 4;                      // 152

constexpr int16_t kSevChip = 48;

constexpr size_t kStatusRowsPerPage = 16;

constexpr int16_t kSettingsRowsPerPage = 8;
constexpr int16_t kSettingsRowTop = kBodyTop;                            // 152
constexpr int16_t kSettingsRowPitch = 58;
constexpr int16_t kSettingsRowH = 54;
constexpr int16_t kSettingsValueX0 = kContentX + kContentW / 2;           // 276
constexpr int16_t kSettingsValueX1 = kContentX + kContentW - kRowPadX - 16 - 8;
constexpr int16_t kSettingsPagerY =
    kSettingsRowTop + kSettingsRowsPerPage * kSettingsRowPitch + 4;      // 620
constexpr int16_t kSettingsPagerH = 48;
static_assert(kSettingsPagerY + kSettingsPagerH <= kContentBottom,
              "S4 rows + pager must end above the soft-key bar");
}  // namespace geom

struct StatusBarModel {
  char timeText[24] = "--:--";
  DeviceState state = DeviceState::Running;
  char stateText[96] = "";
  char stateShort[64] = "";
  uint8_t batPercent = 0;
  uint16_t batMv = 0;
  bool charging = false;
  int wifiState = 0;
  bool mains = true;
  bool portalOpen = false;
  bool apFallback = false;
  bool bleLinked = false;
  size_t mutedCount = 0;
  char banner[96] = "";
};

struct SoftKeyModel {
  const char* label = "";
  bool enabled = true;
};

struct AlertDetailModel {
  uint8_t severity = 0;  // 0 info / 1 warning / 2 critical
  uint8_t state = 0;
  char position[16] = "";
  std::string typeName;
  std::string title;
  std::string message;
  std::vector<std::pair<std::string, std::string>> meta;
  std::vector<std::pair<std::string, std::string>> fields;
  bool empty = false;
};

struct KvModel {
  std::vector<std::pair<std::string, std::string>> rows;
  std::vector<bool> big;
  char pageText[16] = "";
  bool configValid = true;
};

struct OnboardingModel {
  std::string url;
  std::string network;
  bool apMode = false;
  std::string reason;
  bool asleep = false;
};

struct S4RowModel {
  std::string label;
  std::string value;
  bool isAction = false;
};

struct S4Model {
  std::vector<S4RowModel> rows;
  size_t selected = 0;
  size_t page = 0;
  size_t pageCount = 1;
};

struct BlePairModel {
  char passkey[8] = "";      // "012345"
  char deviceName[24] = "";  // EEGO-A4-XXXX
  uint32_t remainingS = 0;
};

struct ProvisionModel {
  char windowText[64] = "";
  char statusText[128] = "";
  bool succeeded = false;
};

void drawStatusBar(eego::PortraitCanvas& c, const StatusBarModel& m);
void drawSoftKeys(eego::PortraitCanvas& c, const SoftKeyModel* keys, size_t n,
                  size_t focused);

void drawOnboarding(eego::PortraitCanvas& c, const OnboardingModel& m);
void drawAlertDetail(eego::PortraitCanvas& c, const AlertDetailModel& m);
void drawStatus(eego::PortraitCanvas& c, const KvModel& m);
void drawS4(eego::PortraitCanvas& c, const S4Model& m);
void drawHelp(eego::PortraitCanvas& c, const KvModel& m);
void drawProvision(eego::PortraitCanvas& c, const ProvisionModel& m);
void drawBlePair(eego::PortraitCanvas& c, const BlePairModel& m);
void drawPoweredOff(eego::PortraitCanvas& c);

void sanitizeClamped(std::string& s, size_t maxChars);

int16_t drawTextFit(eego::PortraitCanvas& c, int16_t x, int16_t y, int16_t maxW,
                    const char* text, int16_t scale, bool bold, uint8_t gray = 0);

int16_t drawTextWrapped(eego::PortraitCanvas& c, int16_t x, int16_t y, int16_t maxW,
                        const char* text, int16_t scale, int16_t lineH, int16_t maxLines,
                        uint8_t gray = 0, bool bold = false);

int16_t lineHeight(int16_t scale);

uint8_t severityFillGray(uint8_t severity);
uint8_t severityTextGray(uint8_t severity);

}  // namespace panel
