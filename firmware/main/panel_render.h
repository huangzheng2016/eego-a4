// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// panel_render —— S0 总览 / S1 表格 / S2 详情 / S3 状态 / S4 设置 的纯绘制层。
//
// 边界（M12：app_task 唯一 UI 属主）：
//   * 本层只做「模型 → 画布」绘制，不持有业务状态、不访问网络/store；
//   * 模型由 panel_app 从不可变快照（SnapshotPublisher acquire）与线程安全
//     查询构建；所有来自网络/库的不可信字符串在构建模型与本层绘制时
//     双重钳制（去控制字符、UTF-8 边界截断、宽度省略号），防止超长/畸形
//     字段撑爆布局；
//   * 灰阶语义：严重度用四级灰阶表达（critical=黑底白字、warning=深灰底
//     白字、info=浅灰底黑字），选中/聚焦用黑框 + 浅灰底，禁用文字用中灰。
//
// 布局（552×768 竖屏，全部内容在普通 UI 安全区 28,28,496×712 内）：
//   状态栏 28..72 ｜ 横幅 76..104 ｜ 标题 108..148 ｜ 正文 152..668 ｜
//   软键栏 672..732（M11 竖屏安全区定稿）。
//
// 文字层级（单一嵌入 CJK 位图字体，整数倍放大）：
//   kTitle      2x 粗体黑   —— 页面标题
//   kBody       2x 常规黑   —— 列表标题、设置标签/值、状态栏、软键
//   kBodyStrong 2x 粗体黑   —— 强调正文（列表行标题、时间）
//   kCaption    1x 常规中灰 —— 次要说明（副标题、年龄、页码、标签列）
//   kSmall      1x 常规黑   —— 高密度主文字（详情消息、状态/帮助值列）
//   kHeadline   3x 粗体黑   —— 总览卡片标签、空态大字
//   kDisplay    6x 粗体黑   —— 总览计数大数字

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "eego_ui.h"
#include "ui_text.h"

namespace panel {

struct TextStyle {
  int16_t scale;
  bool bold;
  uint8_t gray;  // 0=黑 … 15=白
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
constexpr TextStyle kDisplay{6, true, 0};
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

// 统一内边距：自由文字距内容区左右缘 kPadX；框内（行/卡片/条）文字距框缘 kRowPadX。
constexpr int16_t kPadX = 12;
constexpr int16_t kRowPadX = 16;
constexpr int16_t kCardPadX = 24;   // 大色块卡片（S0）内边距
constexpr int16_t kInnerX = kContentX + kPadX;                           // 40
constexpr int16_t kInnerW = kContentW - 2 * kPadX;                       // 472
constexpr int16_t kInnerRight = kInnerX + kInnerW;                       // 512

// 横幅（操作结果/确认提示）覆盖内容区顶部，标题在其下方，不被遮挡。
constexpr int16_t kBannerH = 28;
constexpr int16_t kTitleY = kContentTop + kBannerH + 4;                  // 108
constexpr int16_t kTitleH = 40;
constexpr int16_t kBodyTop = kTitleY + kTitleH + 4;                      // 152

// 严重度芯片（S1 列表行与 S2 详情头部共用同一尺寸/文字样式）。
constexpr int16_t kSevChip = 48;

// S0 总览卡片（点击区域 = kOverviewCardsTop..kOverviewCardsBottom）。
constexpr int16_t kCardH = 130;
constexpr int16_t kCardGap = 10;
constexpr int16_t kOverviewCardsTop = kBodyTop;                                   // 152
constexpr int16_t kOverviewCardsBottom = kOverviewCardsTop + 3 * kCardH + 2 * kCardGap;  // 562

// S1 列表：kRowsPerPage 行，每行 kRowH，自 kTableRowTop 起。
constexpr int16_t kRowsPerPage = 6;
constexpr int16_t kTableRowTop = kBodyTop;                               // 152
constexpr int16_t kRowH = (kContentBottom - kTableRowTop) / kRowsPerPage;  // 86
static_assert(kTableRowTop + kRowsPerPage * kRowH <= kContentBottom,
              "S1 rows must end above the soft-key bar");

// S4 设置：每页 kSettingsRowsPerPage 行（= panel_app kSettingsPageRows），
// 行 i 的点击区域 y ∈ [kSettingsRowTop + i*kSettingsRowPitch, +kSettingsRowPitch)，
// 可见行框高 kSettingsRowH；值列右对齐于 [kSettingsValueX0, kSettingsValueX1)。
constexpr int16_t kSettingsRowsPerPage = 8;
constexpr int16_t kSettingsRowTop = kBodyTop;                            // 152
constexpr int16_t kSettingsRowPitch = 58;
constexpr int16_t kSettingsRowH = 54;
constexpr int16_t kSettingsValueX0 = kContentX + kContentW / 2;           // 276
constexpr int16_t kSettingsValueX1 = kContentX + kContentW - kRowPadX - 16 - 8;  // 484（chevron 左侧）
// 页码指示条（内容区底部）：‹ 页码 ›，左右箭头为可点区域。
constexpr int16_t kSettingsPagerY =
    kSettingsRowTop + kSettingsRowsPerPage * kSettingsRowPitch + 4;      // 620
constexpr int16_t kSettingsPagerH = 48;
constexpr int16_t kSettingsArrowW = 88;
constexpr Rect kSettingsPrevArrow{kContentX + kContentW / 2 - 92 - kSettingsArrowW,
                                  kSettingsPagerY, kSettingsArrowW, kSettingsPagerH};  // x 96..184
constexpr Rect kSettingsNextArrow{kContentX + kContentW / 2 + 92, kSettingsPagerY,
                                  kSettingsArrowW, kSettingsPagerH};     // x 368..456
static_assert(kSettingsPagerY + kSettingsPagerH <= kContentBottom,
              "S4 rows + pager must end above the soft-key bar");
}  // namespace geom

// ── 模型（panel_app 构建；render 只读）──────────────────────────────────────

struct StatusBarModel {
  char timeText[24] = "--:--";  // "MM-DD HH:MM"（统一时间源可信）或「时间不可用」
  uint8_t batPercent = 0;
  uint16_t batMv = 0;
  bool charging = false;
  int wifiState = 0;  // eego::net::WifiState 枚举值（int 避免 net_stack 依赖）
  bool mains = true;  // true=常供电 false=电池
  bool portalOpen = false;
  bool apFallback = false;
  size_t mutedCount = 0;  // store 静音记录数（快照内 muted 计数）
  char banner[96] = "";  // 操作结果/二次确认提示（空 = 无）
};

struct SoftKeyModel {
  const char* label = "";
  bool enabled = true;
};

struct S0Model {
  size_t critical = 0;
  size_t warning = 0;
  size_t info = 0;
  size_t activeTotal = 0;  // Active+Acked（已排除 store 静音记录）
  size_t acked = 0;
  size_t muted = 0;        // store 静音记录数（仅展示）
  size_t sourcesOk = 0;
  size_t sourcesTotal = 0;
  char updatedText[24] = "";
  bool empty = true;
};

struct S1RowModel {
  uint8_t severity = 0;  // alert_core::Severity
  uint8_t state = 0;     // alert_core::AlertState
  std::string title;     // 已钳制（≤120 字符 + 去控制字符）
  std::string subtitle;  // 类型名（zh/en）· 源 id
  char age[24] = "";     // 相对时间（刚刚/Nm ago/...）
};

struct S1Model {
  std::vector<S1RowModel> rows;
  size_t total = 0;
  size_t page = 0;
  size_t pageCount = 0;
  size_t selected = 0;  // 页内选中行下标
};

struct S2Model {
  uint8_t severity = 0;
  uint8_t state = 0;
  std::string typeName;
  std::string title;
  std::string message;
  std::vector<std::pair<std::string, std::string>> meta;  // 标签/值（已格式化）
  std::vector<std::pair<std::string, std::string>> fields;
  bool canAck = false;
  bool canClear = false;
  bool muted = false;  // store 级静音（M21：规则命中，记录留库但不进总览/列表）
};

struct S3Model {
  // 预格式化的行（label/value 两段，已钳制）；分节标题以空 value + bold 呈现
  std::vector<std::pair<std::string, std::string>> rows;
  bool configValid = true;
};

struct S4RowModel {
  std::string label;
  std::string value;
  bool isAction = false;  // true = 动作行（轮询/清静音），false = 开关/查看行
};

struct S4Model {
  std::vector<S4RowModel> rows;
  size_t selected = 0;
  size_t page = 0;
  size_t pageCount = 1;
};

struct ProvisionModel {
  char windowText[48] = "";  // 窗口剩余秒
  bool succeeded = false;
};

// ── 绘制 API（每次调用假设画布已按页面需要清空/铺底）────────────────────────

void drawStatusBar(eego::PortraitCanvas& c, const StatusBarModel& m);
void drawSoftKeys(eego::PortraitCanvas& c, const SoftKeyModel* keys, size_t n,
                  size_t focused);

// 页面内容（不含状态栏/软键栏；状态栏横幅由 drawStatusBar 绘制）。
void drawS0(eego::PortraitCanvas& c, const S0Model& m);
void drawS1(eego::PortraitCanvas& c, const S1Model& m);
void drawS2(eego::PortraitCanvas& c, const S2Model& m);
void drawS3(eego::PortraitCanvas& c, const S3Model& m);
void drawS4(eego::PortraitCanvas& c, const S4Model& m);
void drawHelp(eego::PortraitCanvas& c, const S3Model& m);
void drawProvision(eego::PortraitCanvas& c, const ProvisionModel& m);

// ── 钳制工具（构建模型与绘制共用；渲染层对不可信字段的最后一道闸）────────────

// 去控制字符（<0x20/0x7F）并按 UTF-8 字符边界截断到 maxChars 个字符。
void sanitizeClamped(std::string& s, size_t maxChars);

// 按像素宽度截断（省略号 …），返回实际绘制宽度。text 须已 sanitize。
int16_t drawTextFit(eego::PortraitCanvas& c, int16_t x, int16_t y, int16_t maxW,
                    const char* text, int16_t scale, bool bold, uint8_t gray = 0);

// 简单换行绘制（按宽度折行，UTF-8 安全），返回绘制的行数（≤maxLines，
// 超出末尾画 …）。text 须已 sanitize。
int16_t drawTextWrapped(eego::PortraitCanvas& c, int16_t x, int16_t y, int16_t maxW,
                        const char* text, int16_t scale, int16_t lineH, int16_t maxLines,
                        uint8_t gray = 0, bool bold = false);

// 行高 = fontH(scale) + 4*scale 行距（所有多行文字统一由此推导，避免叠行）。
int16_t lineHeight(int16_t scale);

// 严重度灰阶：底色（0/5/9）与文字色（15=白 / 0=黑）。
uint8_t severityFillGray(uint8_t severity);
uint8_t severityTextGray(uint8_t severity);

}  // namespace panel
