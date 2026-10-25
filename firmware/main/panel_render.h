// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// panel_render —— 设备本地屏幕（引导 / 告警详情 / 状态 / 设置 / 帮助 / 配网）
// 与状态栏、软键栏的纯绘制层。Dashboard 主屏由 dashboard_ui 渲染器绘制。
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
//   kHeadline   3x 粗体黑   —— 引导页设备地址

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

// 统一内边距：自由文字距内容区左右缘 kPadX；框内（行/条）文字距框缘 kRowPadX。
constexpr int16_t kPadX = 12;
constexpr int16_t kRowPadX = 16;
constexpr int16_t kInnerX = kContentX + kPadX;                           // 40
constexpr int16_t kInnerW = kContentW - 2 * kPadX;                       // 472
constexpr int16_t kInnerRight = kInnerX + kInnerW;                       // 512

// 横幅（操作结果/确认提示）覆盖内容区顶部，标题在其下方，不被遮挡。
constexpr int16_t kBannerH = 28;
constexpr int16_t kTitleY = kContentTop + kBannerH + 4;                  // 108
constexpr int16_t kTitleH = 40;
constexpr int16_t kBodyTop = kTitleY + kTitleH + 4;                      // 152

// 严重度芯片（告警详情头部）。
constexpr int16_t kSevChip = 48;

// 状态页每子页最多行数（分节标题计一行；drawKvList 另按内容区底边截断）。
constexpr size_t kStatusRowsPerPage = 16;

// S4 设置：每页 kSettingsRowsPerPage 行（= panel_app kSettingsPageRows），
// 行 i 的点击区域 y ∈ [kSettingsRowTop + i*kSettingsRowPitch, +kSettingsRowPitch)，
// 可见行框高 kSettingsRowH；值列右对齐于 [kSettingsValueX0, kSettingsValueX1)。
constexpr int16_t kSettingsRowsPerPage = 8;
constexpr int16_t kSettingsRowTop = kBodyTop;                            // 152
constexpr int16_t kSettingsRowPitch = 58;
constexpr int16_t kSettingsRowH = 54;
constexpr int16_t kSettingsValueX0 = kContentX + kContentW / 2;           // 276
constexpr int16_t kSettingsValueX1 = kContentX + kContentW - kRowPadX - 16 - 8;  // 484（chevron 左侧）
// 页码指示条（内容区底部）：居中「第 n/N 页」，纯显示（翻页由软键 上一页/下一页）。
constexpr int16_t kSettingsPagerY =
    kSettingsRowTop + kSettingsRowsPerPage * kSettingsRowPitch + 4;      // 620
constexpr int16_t kSettingsPagerH = 48;
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
  size_t mutedCount = 0;  // 外部告警 store 静音记录数（快照内 muted 计数）
  char banner[96] = "";  // 操作结果/二次确认提示（空 = 无）
};

struct SoftKeyModel {
  const char* label = "";
  bool enabled = true;
};

// 告警详情（dashboard 告警视图的一条；外部告警附带 store 元信息）。
struct AlertDetailModel {
  uint8_t severity = 0;  // 0 info / 1 warning / 2 critical
  uint8_t state = 0;     // 0 活动 / 1 已确认 / 2 已恢复 / 3 已过期
  char position[16] = "";  // "2/5"（列表内位置）
  std::string typeName;
  std::string title;
  std::string message;
  std::vector<std::pair<std::string, std::string>> meta;  // 标签/值（已格式化）
  std::vector<std::pair<std::string, std::string>> fields;
  bool empty = false;  // 列表已空（目标消失）
};

// 状态页 / 帮助页：预格式化的行（label/value 两段，已钳制）；分节标题以空
// value 呈现。
struct KvModel {
  std::vector<std::pair<std::string, std::string>> rows;
  char pageText[16] = "";  // 多子页时 "1/2"
  bool configValid = true;
};

// 未配置 dashboard 时的主页：引导到 web 配置。
struct OnboardingModel {
  std::string url;       // http://<ip>/（空 = 尚无地址）
  std::string network;   // WiFi 状态行（SSID · RSSI 或 未连接）
  bool apMode = false;   // 配网/恢复热点开启：引导连接热点
  std::string reason;    // dashboard 配置存在但无效时的原因（已钳制）
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
void drawOnboarding(eego::PortraitCanvas& c, const OnboardingModel& m);
void drawAlertDetail(eego::PortraitCanvas& c, const AlertDetailModel& m);
void drawStatus(eego::PortraitCanvas& c, const KvModel& m);
void drawS4(eego::PortraitCanvas& c, const S4Model& m);
void drawHelp(eego::PortraitCanvas& c, const KvModel& m);
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
