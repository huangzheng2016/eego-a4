// SPDX-License-Identifier: GPL-3.0-or-later
// 设备本地屏幕与状态栏/软键栏绘制实现（见 panel_render.h 布局与边界契约）。
#include "panel_render.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace panel {

namespace {

using eego::FontManager;
using eego::PortraitCanvas;

constexpr uint8_t kDividerGray = 9;   // 行间分隔线（浅灰，白底上可见）
constexpr uint8_t kRuleGray = 6;      // 分节标题下划线（深灰）
constexpr int16_t kKvLabelW = 136;    // 状态/帮助页标签列宽
constexpr int16_t kMetaLabelW = 72;   // 详情页元信息标签列宽
constexpr int16_t kFieldLabelW = 150; // 详情页字段表标签列宽

int16_t textW(const char* s, int16_t scale) {
  return static_cast<int16_t>(FontManager::instance().uiTextWidth(s) * scale);
}

int16_t fontH(int16_t scale) {
  return static_cast<int16_t>(FontManager::instance().uiFontHeight() * scale);
}

// 伪粗体：同色偏移 max(1, scale) px 叠画，额外占用的宽度。
int16_t boldExtra(bool bold, int16_t scale) {
  return bold ? std::max<int16_t>(1, scale) : 0;
}

int16_t styleW(const char* s, const TextStyle& st) {
  return static_cast<int16_t>(textW(s, st.scale) + boldExtra(st.bold, st.scale));
}

// 在高度 h 的带内垂直居中某字号文字的 y（文字顶部）。
int16_t centerY(int16_t top, int16_t h, int16_t scale) {
  return static_cast<int16_t>(top + (h - fontH(scale)) / 2);
}

void textGray(PortraitCanvas& c, int16_t x, int16_t y, const char* s, int16_t scale,
              uint8_t gray, bool bold = false) {
  FontManager::instance().drawTextGray(c, x, y, s, scale, gray);
  if (bold) {
    FontManager::instance().drawTextGray(c, static_cast<int16_t>(x + boldExtra(true, scale)), y,
                                         s, scale, gray);
  }
}

void text(PortraitCanvas& c, int16_t x, int16_t y, const char* s, const TextStyle& st) {
  textGray(c, x, y, s, st.scale, st.gray, st.bold);
}

int16_t textFit(PortraitCanvas& c, int16_t x, int16_t y, int16_t maxW, const char* s,
                const TextStyle& st) {
  return drawTextFit(c, x, y, maxW, s, st.scale, st.bold, st.gray);
}

// 右缘对齐于 right（文字不超过 maxW；超宽省略号）。
void textRight(PortraitCanvas& c, int16_t right, int16_t y, int16_t maxW, const char* s,
               const TextStyle& st) {
  const int16_t tw = std::min(styleW(s, st), maxW);
  textFit(c, static_cast<int16_t>(right - tw), y, maxW, s, st);
}

// 在 [x, x+w) 内水平居中。
void textCentered(PortraitCanvas& c, int16_t x, int16_t w, int16_t y, const char* s,
                  const TextStyle& st) {
  const int16_t tw = std::min(styleW(s, st), w);
  textFit(c, static_cast<int16_t>(x + (w - tw) / 2), y, w, s, st);
}

// 1px 灰阶水平线（fillRoundRectGray r=0 即实心灰矩形）。
void hline(PortraitCanvas& c, int16_t x, int16_t y, int16_t w, uint8_t gray) {
  c.fillRoundRectGray(x, y, w, 1, 0, gray);
}

// UTF-8 字符字节数（按首字节判别；畸形字节按 1 处理，绝不越界读）。
size_t utf8CharLen(unsigned char lead) {
  if (lead < 0x80) return 1;
  if ((lead & 0xE0) == 0xC0) return 2;
  if ((lead & 0xF0) == 0xE0) return 3;
  if ((lead & 0xF8) == 0xF0) return 4;
  return 1;
}

// 严重度短标签（芯片内大字；zh/en 双语）。
const char* sevShort(uint8_t sev) {
  const bool zh = (currentLang() == Lang::Zh);
  switch (sev) {
    case 2: return zh ? "严" : "C";
    case 1: return zh ? "警" : "W";
    default: return zh ? "示" : "I";
  }
}

const char* stateLabel(uint8_t st) {
  switch (st) {
    case 1: return tr(Str::StateAcked);
    case 2: return tr(Str::StateCleared);
    case 3: return tr(Str::StateExpired);
    default: return tr(Str::StateActive);
  }
}

// 页面标题（标题带 kTitleY..+kTitleH 内垂直居中）。
void drawTitleText(PortraitCanvas& c, const char* title, int16_t maxW) {
  textFit(c, geom::kInnerX, centerY(geom::kTitleY, geom::kTitleH, style::kTitle.scale),
          maxW, title, style::kTitle);
}

void drawPageTitle(PortraitCanvas& c, Str title) {
  drawTitleText(c, tr(title), geom::kInnerW);
}

// severity 芯片（kSevChip 方形圆角灰底 + 正文粗体短标签），S1/S2 共用。
void drawSevChip(PortraitCanvas& c, int16_t x, int16_t y, uint8_t sev) {
  const int16_t size = geom::kSevChip;
  c.fillRoundRectGray(x, y, size, size, 8, severityFillGray(sev));
  TextStyle st = style::kBodyStrong;
  if (fontH(st.scale) > size - 8) st.scale = 1;
  st.gray = severityTextGray(sev);
  const int16_t tw = styleW(sevShort(sev), st);
  text(c, static_cast<int16_t>(x + (size - tw) / 2), centerY(y, size, st.scale), sevShort(sev),
       st);
}

// ── 状态栏图标（按字高缩放的矢量近似）────────────────────────────────────────

// 扫描线填充多边形（偶奇规则，采样像素中心）；span(x0, x1, y) 画 [x0, x1] 闭区间。
template <typename Span>
void scanPolygon(const float* px, const float* py, int n, int16_t y0, int16_t y1, Span&& span) {
  float xs[16];
  for (int16_t yy = y0; yy < y1; yy++) {
    const float yc = static_cast<float>(yy) + 0.5f;
    int k = 0;
    for (int i = 0; i < n && k < 16; i++) {
      const int j = (i + 1) % n;
      const float ya = py[i];
      const float yb = py[j];
      if ((ya <= yc && yb > yc) || (yb <= yc && ya > yc)) {
        xs[k++] = px[i] + (yc - ya) * (px[j] - px[i]) / (yb - ya);
      }
    }
    std::sort(xs, xs + k);
    for (int i = 0; i + 1 < k; i += 2) {
      const int16_t a = static_cast<int16_t>(xs[i] + 0.5f);
      const int16_t b = static_cast<int16_t>(xs[i + 1] - 0.5f);
      if (b >= a) span(a, b, yy);
    }
  }
}

// 闪电：单位方框内 7 点多边形（右上起笔 → 左下折 → 中段横移 → 底尖 → 回折）。
constexpr float kBoltX[7] = {0.62f, 0.02f, 0.44f, 0.26f, 0.98f, 0.56f, 0.90f};
constexpr float kBoltY[7] = {0.00f, 0.58f, 0.58f, 1.00f, 0.40f, 0.40f, 0.00f};

int16_t batteryIconWidth(int16_t h) { return static_cast<int16_t>(h * 2 + 3); }

// 电池图标：h 高、2h 宽、2px 描边 + 3px 正极凸点；内芯按百分比填充；
// bolt=true 时内部画闪电（填充上为白、空白处为黑）。返回总宽。
int16_t drawBatteryIcon(PortraitCanvas& c, int16_t x, int16_t y, int16_t h, uint8_t percent,
                        bool bolt) {
  const int16_t bw = static_cast<int16_t>(h * 2);
  c.drawRect(x, y, bw, h, 0);
  c.drawRect(static_cast<int16_t>(x + 1), static_cast<int16_t>(y + 1),
             static_cast<int16_t>(bw - 2), static_cast<int16_t>(h - 2), 0);
  const int16_t nubH = std::max<int16_t>(4, static_cast<int16_t>(h * 3 / 7));
  c.fillRect(static_cast<int16_t>(x + bw), static_cast<int16_t>(y + (h - nubH) / 2), 3, nubH, 0);

  const int16_t ix = static_cast<int16_t>(x + 4);
  const int16_t iy = static_cast<int16_t>(y + 4);
  const int16_t iw = static_cast<int16_t>(bw - 8);
  const int16_t ih = static_cast<int16_t>(h - 8);
  const int pct = std::min<int>(percent, 100);
  int16_t fillW = static_cast<int16_t>((iw * pct + 50) / 100);
  if (pct > 0 && fillW < 1) fillW = 1;
  if (fillW > 0 && ih > 0) c.fillRect(ix, iy, fillW, ih, 0);

  if (bolt) {
    // 闪电占据描边内的整个高度（含 2px 留白），宽 ≈ 高
    const int16_t boltH = static_cast<int16_t>(h - 4);
    const int16_t boltW = static_cast<int16_t>(boltH + 1);
    const float bx = static_cast<float>(x + (bw - boltW) / 2);
    const float by = static_cast<float>(y + 2);
    float px[7];
    float py[7];
    for (int i = 0; i < 7; i++) {
      px[i] = bx + kBoltX[i] * static_cast<float>(boltW);
      py[i] = by + kBoltY[i] * static_cast<float>(boltH);
    }
    const int16_t fillEnd = static_cast<int16_t>(ix + fillW);
    scanPolygon(px, py, 7, static_cast<int16_t>(y + 2), static_cast<int16_t>(y + 2 + boltH),
                [&](int16_t a, int16_t b, int16_t yy) {
                  const bool rowInFill = yy >= iy && yy < iy + ih;
                  for (int16_t xx = a; xx <= b; xx++) {
                    const bool onFill = rowInFill && xx >= ix && xx < fillEnd;
                    c.drawPixel(xx, yy, onFill ? 1 : 0);
                  }
                });
  }
  return batteryIconWidth(h);
}

int16_t wifiIconWidth(int16_t h) { return static_cast<int16_t>(h * 13 / 10 + 1); }

// WiFi 扇形：底部中心为原点，±45° 内一个圆点 + 两道弧。
bool wifiBandHit(float dx, float dy, float h) {
  if (dy <= 0.0f) return false;
  const float r2 = dx * dx + dy * dy;
  const float dot = 0.20f * h;
  if (r2 <= dot * dot) return true;
  const float ax = dx < 0.0f ? -dx : dx;
  if (ax > dy) return false;  // ±45° 扇区外
  const float a0 = 0.40f * h, a1 = 0.60f * h;
  const float b0 = 0.76f * h, b1 = 0.97f * h;
  return (r2 >= a0 * a0 && r2 <= a1 * a1) || (r2 >= b0 * b0 && r2 <= b1 * b1);
}

// state：0 未连接 1 连接中 2 已连接 3 失败 4 离线（eego::net::WifiState）。
// 已连接=实心；连接中=空心描边；其他=空心 + 斜杠。返回宽度。
int16_t drawWifiIcon(PortraitCanvas& c, int16_t x, int16_t y, int16_t h, int state) {
  const int16_t w = wifiIconWidth(h);
  const float ox = static_cast<float>(w) / 2.0f;
  const float oy = static_cast<float>(h) - 0.5f;
  const float fh = static_cast<float>(h);
  const bool solid = (state == 2);
  auto hit = [&](int16_t xx, int16_t yy) {
    if (xx < 0 || yy < 0 || xx >= w || yy >= h) return false;
    return wifiBandHit(static_cast<float>(xx) + 0.5f - ox, oy - (static_cast<float>(yy) + 0.5f),
                       fh);
  };
  for (int16_t yy = 0; yy < h; yy++) {
    for (int16_t xx = 0; xx < w; xx++) {
      if (!hit(xx, yy)) continue;
      const bool edge = !hit(static_cast<int16_t>(xx - 1), yy) ||
                        !hit(static_cast<int16_t>(xx + 1), yy) ||
                        !hit(xx, static_cast<int16_t>(yy - 1)) ||
                        !hit(xx, static_cast<int16_t>(yy + 1));
      if (solid || edge) {
        c.drawPixel(static_cast<int16_t>(x + xx), static_cast<int16_t>(y + yy), 0);
      }
    }
  }
  if (state != 1 && !solid) {
    // 斜杠：白色描边隔开扇形后再画 2px 黑线
    const int16_t x1 = static_cast<int16_t>(x + w - 1);
    const int16_t y1 = static_cast<int16_t>(y + h - 1);
    c.drawLine(static_cast<int16_t>(x - 1), y, static_cast<int16_t>(x1 - 1), y1, 1);
    c.drawLine(static_cast<int16_t>(x + 2), y, static_cast<int16_t>(x1 + 2), y1, 1);
    c.drawLine(x, y, x1, y1, 0);
    c.drawLine(static_cast<int16_t>(x + 1), y, static_cast<int16_t>(x1 + 1), y1, 0);
  }
  return w;
}

// 状态/帮助页的「分节标题 + 标签/值」列表（S3 与帮助页共用同一排版）。
int16_t drawKvList(PortraitCanvas& c, const std::vector<std::pair<std::string, std::string>>& rows,
                   const std::vector<bool>& big, int16_t y) {
  const int16_t lh = lineHeight(style::kSmall.scale);
  const int16_t fh = fontH(style::kSmall.scale);
  for (size_t i = 0; i < rows.size(); ++i) {
    const auto& row = rows[i];
    const bool large = i < big.size() && big[i];
    if (large && row.second.empty()) {
      // 正文字号分节标题（粗体）+ 深灰下划线（状态页）。
      const int16_t fhBig = fontH(style::kBodyStrong.scale);
      if (y + fhBig > geom::kContentBottom) break;
      textFit(c, geom::kInnerX, y, geom::kInnerW, row.first.c_str(), style::kBodyStrong);
      hline(c, geom::kInnerX, static_cast<int16_t>(y + fhBig + 3), geom::kInnerW, kRuleGray);
      y = static_cast<int16_t>(y + fhBig + 12);
      continue;
    }
    if (large && !row.second.empty()) {
      // 正文字号行：标签深灰、值黑色，标签列加宽（英文标签较长）。
      const int16_t lhBig = lineHeight(style::kBody.scale);
      if (y + fontH(style::kBody.scale) > geom::kContentBottom) break;
      const int16_t labelW = kKvLabelW + 16;
      textFit(c, geom::kInnerX, y, static_cast<int16_t>(labelW - 8), row.first.c_str(),
              style::kBodyMuted);
      textFit(c, static_cast<int16_t>(geom::kInnerX + labelW), y,
              static_cast<int16_t>(geom::kInnerW - labelW), row.second.c_str(), style::kBody);
      y = static_cast<int16_t>(y + lhBig);
      continue;
    }
    if (y + fh > geom::kContentBottom) break;
    if (row.second.empty()) {
      // 分节标题 + 深灰下划线
      textFit(c, geom::kInnerX, y, geom::kInnerW, row.first.c_str(), style::kCaptionStrong);
      hline(c, geom::kInnerX, static_cast<int16_t>(y + fh + 3), geom::kInnerW, kRuleGray);
      y = static_cast<int16_t>(y + fh + 10);
    } else {
      textFit(c, geom::kInnerX, y, static_cast<int16_t>(kKvLabelW - 8), row.first.c_str(),
              style::kCaption);
      textFit(c, static_cast<int16_t>(geom::kInnerX + kKvLabelW), y,
              static_cast<int16_t>(geom::kInnerW - kKvLabelW), row.second.c_str(),
              style::kSmall);
      y = static_cast<int16_t>(y + lh);
    }
  }
  return y;
}

}  // namespace

// ── 钳制工具 ────────────────────────────────────────────────────────────────

void sanitizeClamped(std::string& s, size_t maxChars) {
  std::string out;
  out.reserve(s.size());
  size_t chars = 0;
  size_t i = 0;
  while (i < s.size() && chars < maxChars) {
    const unsigned char b = static_cast<unsigned char>(s[i]);
    if (b < 0x20 || b == 0x7F) {  // 控制字符直接剔除（不换行不显示）
      i++;
      continue;
    }
    const size_t len = utf8CharLen(b);
    if (i + len > s.size()) break;  // 畸形尾序列，丢弃
    out.append(s, i, len);
    i += len;
    chars++;
  }
  s.swap(out);
}

int16_t lineHeight(int16_t scale) {
  return static_cast<int16_t>(fontH(scale) + 4 * scale);
}

int16_t drawTextFit(PortraitCanvas& c, int16_t x, int16_t y, int16_t maxW,
                    const char* text, int16_t scale, bool bold, uint8_t gray) {
  if (text == nullptr || *text == '\0' || maxW <= 0) return 0;
  const int16_t extra = boldExtra(bold, scale);
  std::string s(text);
  if (textW(s.c_str(), scale) + extra <= maxW) {
    textGray(c, x, y, s.c_str(), scale, gray, bold);
    return static_cast<int16_t>(textW(s.c_str(), scale) + extra);
  }
  // 逐个字符回退直到 "..." 放得下（n≤120，O(n²) 可接受）
  while (!s.empty()) {
    std::string t = s + "...";
    if (textW(t.c_str(), scale) + extra <= maxW) {
      textGray(c, x, y, t.c_str(), scale, gray, bold);
      return static_cast<int16_t>(textW(t.c_str(), scale) + extra);
    }
    // 去最后一个 UTF-8 字符
    size_t cut = s.size() - 1;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) cut--;
    s.erase(cut);
  }
  textGray(c, x, y, "...", scale, gray, bold);
  return static_cast<int16_t>(textW("...", scale) + extra);
}

int16_t drawTextWrapped(PortraitCanvas& c, int16_t x, int16_t y, int16_t maxW,
                        const char* text, int16_t scale, int16_t lineH, int16_t maxLines,
                        uint8_t gray, bool bold) {
  if (text == nullptr || *text == '\0' || maxLines <= 0) return 0;
  const int16_t fitW = static_cast<int16_t>(maxW - boldExtra(bold, scale));
  std::string s(text);
  int16_t line = 0;
  size_t pos = 0;
  while (pos < s.size() && line < maxLines) {
    // 收集本行最多字符
    size_t end = pos;
    size_t lastSpace = std::string::npos;
    while (end < s.size()) {
      const unsigned char b = static_cast<unsigned char>(s[end]);
      const size_t len = utf8CharLen(b);
      std::string probe = s.substr(pos, end - pos + len);
      if (textW(probe.c_str(), scale) > fitW) break;
      if (b == ' ') lastSpace = end;
      end += len;
    }
    if (end == pos) end = s.size();  // 单字符超宽：兜底全画（裁剪区保护）
    const bool lastLine = (line == maxLines - 1);
    const bool more = end < s.size();
    std::string cur = s.substr(pos, end - pos);
    if (lastSpace != std::string::npos && lastSpace > pos && end < s.size()) {
      cur = s.substr(pos, lastSpace - pos);  // 优先在空格处折行
      end = lastSpace + 1;
    }
    if (lastLine && more) {
      // 末行截断加省略号
      while (!cur.empty() && textW((cur + "...").c_str(), scale) > fitW) {
        size_t cut = cur.size() - 1;
        while (cut > 0 && (static_cast<unsigned char>(cur[cut]) & 0xC0) == 0x80) cut--;
        cur.erase(cut);
      }
      cur += "...";
    }
    textGray(c, x, static_cast<int16_t>(y + line * lineH), cur.c_str(), scale, gray, bold);
    line++;
    pos = end;
    if (!lastLine && pos >= s.size()) break;
  }
  return line;
}

uint8_t severityFillGray(uint8_t severity) {
  switch (severity) {
    case 2: return 0;   // critical：黑
    case 1: return 5;   // warning：深灰
    default: return 9;  // info：浅灰
  }
}

uint8_t severityTextGray(uint8_t severity) {
  return severity == 0 ? 0 : 15;  // 浅灰底配黑字，其余白字
}

// ── 状态栏 ──────────────────────────────────────────────────────────────────
//
// 左：时间；右（自右向左）：电量% · 6px · 电池 · 12px · WiFi（未连接时其左侧附状态文字）；
// 中：配网/AP 徽章与静音计数，放不下则依次省略。全部在 kStatusBarH 内垂直居中。

void drawStatusBar(PortraitCanvas& c, const StatusBarModel& m) {
  const int16_t x = geom::kContentX;
  const int16_t y = geom::kContentY;
  const int16_t w = geom::kContentW;
  const int16_t barH = static_cast<int16_t>(geom::kStatusBarH - 1);  // 末行为分隔线
  c.fillRect(x, y, w, geom::kStatusBarH, 1);  // 白底
  c.drawLine(x, static_cast<int16_t>(y + barH), static_cast<int16_t>(x + w - 1),
             static_cast<int16_t>(y + barH), 0);

  TextStyle body = style::kBody;
  if (fontH(body.scale) > barH - 4) body.scale = 1;
  TextStyle strong = body;
  strong.bold = true;
  TextStyle muted = body;
  muted.gray = 6;
  const int16_t ty = centerY(y, barH, body.scale);
  // 图标高 ≈ 状态文字大写/数字字高（行高的 ~1/2）
  const int16_t capH = std::max<int16_t>(12, static_cast<int16_t>(fontH(body.scale) / 2));

  // 左：时间
  const int16_t leftEnd = static_cast<int16_t>(
      geom::kInnerX + drawTextFit(c, geom::kInnerX, ty, static_cast<int16_t>(w / 2 - geom::kPadX),
                                  m.timeText, strong.scale, strong.bold, strong.gray));

  // 右：电量% → 电池 → WiFi
  int16_t rx = geom::kInnerRight;
  const bool hasBattery = m.batMv > 0;  // 0 mV = 无电池读数（常供电）
  if (hasBattery) {
    char bat[8];
    snprintf(bat, sizeof(bat), "%u%%", static_cast<unsigned>(std::min<int>(m.batPercent, 100)));
    rx = static_cast<int16_t>(rx - styleW(bat, body));
    text(c, rx, ty, bat, body);
    rx = static_cast<int16_t>(rx - 6);
  }
  const int16_t batH = static_cast<int16_t>(capH & ~1);
  rx = static_cast<int16_t>(rx - batteryIconWidth(batH));
  // 无电池（纯外接供电）= 满格 + 闪电；充电 = 实际电量 + 闪电
  drawBatteryIcon(c, rx, static_cast<int16_t>(y + (barH - batH) / 2), batH,
                  hasBattery ? m.batPercent : 100, m.charging || !hasBattery);
  rx = static_cast<int16_t>(rx - 12);

  const int16_t wifiH = static_cast<int16_t>(capH + 2);
  rx = static_cast<int16_t>(rx - wifiIconWidth(wifiH));
  drawWifiIcon(c, rx, static_cast<int16_t>(y + (barH - wifiH) / 2), wifiH, m.wifiState);
  if (m.wifiState != 2) {
    const char* wifi = tr(Str::WifiDisconnected);
    switch (m.wifiState) {
      case 1: wifi = tr(Str::WifiConnecting); break;
      case 3: wifi = tr(Str::WifiFailed); break;
      case 4: wifi = tr(Str::WifiOffline); break;
      default: break;
    }
    const int16_t avail = static_cast<int16_t>(rx - 6 - (leftEnd + 12));
    const int16_t maxW = std::min<int16_t>(avail, 160);
    if (maxW >= styleW("...", muted) + textW("W", muted.scale)) {
      const int16_t lw = std::min(styleW(wifi, muted), maxW);
      rx = static_cast<int16_t>(rx - 6 - lw);
      textFit(c, rx, ty, lw, wifi, muted);
    }
  }

  // 中（左对齐）：配网/AP 胶囊（黑底白字）→ 设备状态（图标 + 正文字号标签）→ 静音
  // 计数（灰字）。放不下时先省略静音，再截断状态标签，最后省略状态。
  const int16_t mx0 = static_cast<int16_t>(leftEnd + 16);
  const int16_t mx1 = static_cast<int16_t>(rx - 12);
  const char* portal = m.portalOpen ? tr(Str::PortalOpen)
                                    : (m.apFallback ? tr(Str::ApFallback) : nullptr);
  char mute[24] = "";
  if (m.mutedCount > 0) {
    snprintf(mute, sizeof(mute), "%s%u", tr(Str::Mute), static_cast<unsigned>(m.mutedCount));
  }
  const int16_t pillPad = 8;
  const int16_t portalW =
      portal != nullptr ? static_cast<int16_t>(styleW(portal, strong) + 2 * pillPad) : 0;
  int16_t bx = mx0;
  if (portalW > 0 && bx + portalW <= mx1) {
    const int16_t pillH = std::min<int16_t>(static_cast<int16_t>(fontH(body.scale) + 4),
                                            static_cast<int16_t>(barH - 4));
    const int16_t py = static_cast<int16_t>(y + (barH - pillH) / 2);
    c.fillRoundRectGray(bx, py, portalW, pillH, static_cast<int16_t>(pillH / 2), 0);
    TextStyle onDark = strong;
    onDark.gray = 15;
    text(c, static_cast<int16_t>(bx + pillPad), ty, portal, onDark);
    bx = static_cast<int16_t>(bx + portalW + 10);
  }
  const int16_t muteW = mute[0] != '\0' ? styleW(mute, muted) : 0;
  if (m.stateText[0] != '\0') {
    const int16_t icon = stateIconSize(fontH(body.scale));
    const char* label = m.stateText;
    if (m.stateShort[0] != '\0' &&
        styleW(m.stateText, body) > static_cast<int16_t>(mx1 - bx - icon - 6)) {
      label = m.stateShort;
    }
    const int16_t stateW = styleW(label, body);
    int16_t room = static_cast<int16_t>(mx1 - bx - icon - 6);
    const bool muteFits = muteW > 0 && stateW + 10 + muteW <= room;
    if (muteFits) room = static_cast<int16_t>(room - muteW - 10);
    if (room >= styleW("...", body) + textW("W", body.scale)) {
      drawStateIcon(c, bx, static_cast<int16_t>(y + (barH - icon) / 2), icon, m.state);
      const int16_t used = textFit(c, static_cast<int16_t>(bx + icon + 6), ty, room, label, body);
      bx = static_cast<int16_t>(bx + icon + 6 + used + 10);
      if (muteFits) text(c, bx, ty, mute, muted);
    }
  } else if (muteW > 0 && bx + muteW <= mx1) {
    text(c, bx, ty, mute, muted);
  }

  // 横幅（操作结果/二次确认）：内容区顶部 kBannerH 浅灰条（标题在其下方）
  if (m.banner[0] != '\0') {
    c.fillRect(x, geom::kContentTop, w, geom::kBannerH, 1);
    c.fillRoundRectGray(x, geom::kContentTop, w, geom::kBannerH, 4, 11);
    textFit(c, static_cast<int16_t>(x + geom::kRowPadX),
            centerY(geom::kContentTop, geom::kBannerH, style::kCaptionStrong.scale),
            static_cast<int16_t>(w - 2 * geom::kRowPadX), m.banner, style::kCaptionStrong);
  }
}

// ── 软键栏 ──────────────────────────────────────────────────────────────────

void drawSoftKeys(PortraitCanvas& c, const SoftKeyModel* keys, size_t n, size_t focused) {
  if (keys == nullptr || n == 0) return;
  const int16_t gap = geom::kSoftKeyGap;
  const int16_t totalW = geom::kContentW;
  const int16_t kw = static_cast<int16_t>((totalW - gap * static_cast<int16_t>(n - 1)) /
                                          static_cast<int16_t>(n));
  const int16_t inner = static_cast<int16_t>(kw - 12);
  // 整栏统一字号：取所有键都放得下（含选中粗体偏移）的最大倍数，最大 3x
  int16_t scale = 3;
  for (; scale > 1; scale--) {
    if (fontH(scale) > geom::kSoftKeyH - 8) continue;
    bool fits = true;
    for (size_t i = 0; i < n && fits; i++) {
      const char* label = keys[i].label != nullptr ? keys[i].label : "";
      if (textW(label, scale) + boldExtra(true, scale) > inner) fits = false;
    }
    if (fits) break;
  }
  const int16_t ty = centerY(geom::kSoftKeyY, geom::kSoftKeyH, scale);
  for (size_t i = 0; i < n; i++) {
    const int16_t x = static_cast<int16_t>(geom::kContentX + static_cast<int16_t>(i) * (kw + gap));
    const bool sel = (i == focused);
    const char* label = keys[i].label != nullptr ? keys[i].label : "";
    const int16_t tw = static_cast<int16_t>(textW(label, scale) + boldExtra(sel, scale));
    const int16_t tx = static_cast<int16_t>(x + (kw - tw) / 2);
    const int16_t textX = tx < x + 6 ? static_cast<int16_t>(x + 6) : tx;
    if (!keys[i].enabled) {
      // 禁用：浅灰底 + 中灰字（不可用 BW 描边，白边在白底上不可见）
      c.fillRoundRectGray(x, geom::kSoftKeyY, kw, geom::kSoftKeyH, 8, 12);
      drawTextFit(c, textX, ty, inner, label, scale, false, 6);
    } else if (sel) {
      c.fillRoundRectGray(x, geom::kSoftKeyY, kw, geom::kSoftKeyH, 8, 0);
      drawTextFit(c, textX, ty, inner, label, scale, true, 15);
    } else {
      c.drawRoundRect(x, geom::kSoftKeyY, kw, geom::kSoftKeyH, 8, 0);
      drawTextFit(c, textX, ty, inner, label, scale, false, 0);
    }
  }
}

// ── 告警详情 ────────────────────────────────────────────────────────────────

void drawAlertDetail(PortraitCanvas& c, const AlertDetailModel& m) {
  int16_t y = geom::kTitleY;
  if (m.empty) {
    drawPageTitle(c, Str::PageDetail);
    textCentered(c, geom::kInnerX, geom::kInnerW,
                 centerY(geom::kBodyTop, static_cast<int16_t>(geom::kContentBottom - geom::kBodyTop),
                         style::kBodyMuted.scale),
                 tr(Str::EmptyList), style::kBodyMuted);
    return;
  }

  // 头部：严重度芯片 + 类型名（正文粗体，垂直居中于芯片）+ 右侧状态
  drawSevChip(c, geom::kInnerX, y, m.severity);
  const TextStyle stateSt = style::kCaption;
  char stateText[48];
  if (m.position[0] != '\0') {
    snprintf(stateText, sizeof(stateText), "%s · %s", stateLabel(m.state), m.position);
  } else {
    snprintf(stateText, sizeof(stateText), "%s", stateLabel(m.state));
  }
  const int16_t stateW = std::min<int16_t>(styleW(stateText, stateSt), 180);
  textRight(c, geom::kInnerRight, centerY(y, geom::kSevChip, stateSt.scale), stateW, stateText,
            stateSt);
  const int16_t typeX = static_cast<int16_t>(geom::kInnerX + geom::kSevChip + 12);
  textFit(c, typeX, centerY(y, geom::kSevChip, style::kBodyStrong.scale),
          static_cast<int16_t>(geom::kInnerRight - stateW - 16 - typeX), m.typeName.c_str(),
          style::kBodyStrong);
  y = static_cast<int16_t>(y + geom::kSevChip + 12);

  // 标题（正文粗体，最多 2 行）
  const int16_t lh2 = lineHeight(style::kBodyStrong.scale);
  const int16_t titleLines = drawTextWrapped(c, geom::kInnerX, y, geom::kInnerW, m.title.c_str(),
                                             style::kBodyStrong.scale, lh2, 2,
                                             style::kBodyStrong.gray, style::kBodyStrong.bold);
  y = static_cast<int16_t>(y + titleLines * lh2 + 6);

  // 消息（小字，最多 5 行，且不越过正文区底部）
  const int16_t lh1 = lineHeight(style::kSmall.scale);
  const int16_t fh1 = fontH(style::kSmall.scale);
  const int16_t msgMax = std::max<int16_t>(
      0, std::min<int16_t>(5, static_cast<int16_t>((geom::kContentBottom - y) / lh1)));
  const int16_t msgLines = drawTextWrapped(c, geom::kInnerX, y, geom::kInnerW, m.message.c_str(),
                                           style::kSmall.scale, lh1, msgMax, style::kSmall.gray);
  y = static_cast<int16_t>(y + msgLines * lh1 + 8);

  // 元信息两列（最多 8 项）：标签（说明字）+ 值（小字）
  const int16_t colW = static_cast<int16_t>(geom::kInnerW / 2);
  const int16_t metaPitch = lh1;
  size_t i = 0;
  for (; i < m.meta.size() && i < 8; i++) {
    const int16_t cx = static_cast<int16_t>(geom::kInnerX + static_cast<int16_t>(i % 2) * colW);
    const int16_t cy = static_cast<int16_t>(y + static_cast<int16_t>(i / 2) * metaPitch);
    if (cy + fh1 > geom::kContentBottom) break;
    textFit(c, cx, cy, static_cast<int16_t>(kMetaLabelW - 6), m.meta[i].first.c_str(),
            style::kCaption);
    textFit(c, static_cast<int16_t>(cx + kMetaLabelW), cy,
            static_cast<int16_t>(colW - kMetaLabelW - 8), m.meta[i].second.c_str(),
            style::kSmall);
  }
  if (i > 0) {
    y = static_cast<int16_t>(y + static_cast<int16_t>((i + 1) / 2) * metaPitch + 6);
  }

  // 字段表（最多 4 行，浅灰底条）
  const int16_t stripH = static_cast<int16_t>(fh1 + 6);
  for (size_t f = 0; f < m.fields.size() && f < 4; f++) {
    const int16_t fy = static_cast<int16_t>(y + static_cast<int16_t>(f) * (stripH + 2));
    if (fy + stripH > geom::kContentBottom) break;
    c.fillRoundRectGray(geom::kInnerX, fy, geom::kInnerW, stripH, 4, 12);
    const int16_t ty = centerY(fy, stripH, style::kSmall.scale);
    textFit(c, static_cast<int16_t>(geom::kInnerX + 8), ty, static_cast<int16_t>(kFieldLabelW - 8),
            m.fields[f].first.c_str(), style::kCaption);
    textFit(c, static_cast<int16_t>(geom::kInnerX + kFieldLabelW + 8), ty,
            static_cast<int16_t>(geom::kInnerW - kFieldLabelW - 16), m.fields[f].second.c_str(),
            style::kSmall);
  }
}

// ── 状态 ────────────────────────────────────────────────────────────────────

void drawStatus(PortraitCanvas& c, const KvModel& m) {
  drawPageTitle(c, Str::PageStatus);
  if (m.pageText[0] != '\0') {
    textRight(c, geom::kInnerRight, centerY(geom::kTitleY, geom::kTitleH, style::kCaption.scale),
              120, m.pageText, style::kCaption);
  }
  const int16_t y = drawKvList(c, m.rows, m.big, geom::kBodyTop);
  const int16_t stripH = static_cast<int16_t>(fontH(style::kCaptionStrong.scale) + 8);
  if (!m.configValid && y + 4 + stripH <= geom::kContentBottom) {
    const int16_t sy = static_cast<int16_t>(y + 4);
    c.fillRoundRectGray(geom::kInnerX, sy, geom::kInnerW, stripH, 4, 5);
    TextStyle st = style::kCaptionStrong;
    st.gray = 15;
    textFit(c, static_cast<int16_t>(geom::kInnerX + 8), centerY(sy, stripH, st.scale),
            static_cast<int16_t>(geom::kInnerW - 16), tr(Str::ConfigBad), st);
  }
}

void drawHelp(PortraitCanvas& c, const KvModel& m) {
  drawPageTitle(c, Str::PageHelp);
  drawKvList(c, m.rows, m.big, geom::kBodyTop);
}

// ── S4 设置 ─────────────────────────────────────────────────────────────────

void drawS4(PortraitCanvas& c, const S4Model& m) {
  drawPageTitle(c, Str::PageSettings);
  const int16_t x = geom::kContentX;
  const int16_t w = geom::kContentW;
  const int16_t boxX = static_cast<int16_t>(x + 2);
  const int16_t boxW = static_cast<int16_t>(w - 4);
  const int16_t labelX = static_cast<int16_t>(x + geom::kRowPadX);
  const int16_t valueRight = geom::kSettingsValueX1;
  const TextStyle labelSt = style::kBody;
  const TextStyle valueSt = style::kBodyMuted;

  for (size_t i = 0; i < m.rows.size() && i < static_cast<size_t>(geom::kSettingsRowsPerPage);
       i++) {
    const S4RowModel& row = m.rows[i];
    const bool sel = (i == m.selected);
    const int16_t ry = static_cast<int16_t>(geom::kSettingsRowTop +
                                            static_cast<int16_t>(i) * geom::kSettingsRowPitch);
    if (sel) {
      c.fillRoundRectGray(boxX, ry, boxW, geom::kSettingsRowH, 8, 11);
      c.drawRoundRect(boxX, ry, boxW, geom::kSettingsRowH, 8, 0);
    }
    const int16_t ty = centerY(ry, geom::kSettingsRowH, labelSt.scale);

    // 标签与值同字号；值右对齐。总宽不够时标签至多让到一半，值仍放不下则退到小字号
    // （IP/时间等不宜省略），再不够才省略。
    const int16_t avail = static_cast<int16_t>(valueRight - labelX - 16);
    const int16_t lW = styleW(row.label.c_str(), labelSt);
    const int16_t vW = row.value.empty() ? 0 : styleW(row.value.c_str(), valueSt);
    int16_t labelMax = avail;
    if (vW > 0) labelMax = std::max<int16_t>(static_cast<int16_t>(avail / 2),
                                             static_cast<int16_t>(avail - vW));
    const int16_t labelUsed = std::min(lW, labelMax);
    textFit(c, labelX, ty, labelMax, row.label.c_str(), labelSt);
    if (vW > 0) {
      const int16_t valueMax = static_cast<int16_t>(avail - labelUsed);
      TextStyle vs = valueSt;
      if (vW > valueMax && styleW(row.value.c_str(), style::kCaption) <= valueMax) {
        vs.scale = style::kCaption.scale;
      }
      textRight(c, valueRight, centerY(ry, geom::kSettingsRowH, vs.scale), valueMax,
                row.value.c_str(), vs);
    }
    if (row.isAction) {
      // 动作行右侧 › 标记
      c.drawIcon(static_cast<int16_t>(boxX + boxW - geom::kRowPadX - 16),
                 static_cast<int16_t>(ry + (geom::kSettingsRowH - 16) / 2),
                 eego::ICON_CHEVRON_RIGHT_16, 16, 16, 0);
    }
  }

  // 页码指示条：居中「第 n/N 页」（纯显示；翻页走软键 上一页/下一页）
  char page[32];
  snprintf(page, sizeof(page), tr(Str::SettingsPageFmt), static_cast<unsigned>(m.page + 1),
           static_cast<unsigned>(m.pageCount));
  textCentered(c, geom::kContentX, geom::kContentW,
               centerY(geom::kSettingsPagerY, geom::kSettingsPagerH, style::kBody.scale), page,
               style::kBody);
}

// ── 引导页（未配置 dashboard 的主页）────────────────────────────────────────
//
// 标题 ｜ 「在浏览器中打开」+ 地址大字框（设备 IP 或配网热点地址）｜ WiFi 状态 ｜
// 三步说明 ｜ 无效配置原因（如有）｜ 前置键提示（内容区底部）。

void drawOnboarding(PortraitCanvas& c, const OnboardingModel& m) {
  drawPageTitle(c, Str::PageOnboarding);
  int16_t y = geom::kBodyTop;

  textFit(c, geom::kInnerX, y, geom::kInnerW, tr(Str::OnbOpen), style::kCaption);
  y = static_cast<int16_t>(y + lineHeight(style::kCaption.scale) + 4);

  // 地址框：黑框白底，地址按框宽选最大字号（3x → 2x → 1x）。
  const int16_t boxH = 84;
  c.drawRoundRect(geom::kContentX, y, geom::kContentW, boxH, 10, 0);
  c.drawRoundRect(static_cast<int16_t>(geom::kContentX + 1), static_cast<int16_t>(y + 1),
                  static_cast<int16_t>(geom::kContentW - 2), static_cast<int16_t>(boxH - 2), 9, 0);
  const char* address = m.url.empty() ? tr(Str::OnbNoAddress) : m.url.c_str();
  TextStyle addr = m.url.empty() ? style::kBodyMuted : style::kHeadline;
  const int16_t boxInner = static_cast<int16_t>(geom::kContentW - 2 * geom::kRowPadX);
  while (addr.scale > 1 && (styleW(address, addr) > boxInner || fontH(addr.scale) > boxH - 12)) {
    addr.scale = static_cast<int16_t>(addr.scale - 1);
  }
  textCentered(c, geom::kContentX, geom::kContentW, centerY(y, boxH, addr.scale), address, addr);
  y = static_cast<int16_t>(y + boxH + 12);

  if (!m.network.empty()) {
    textFit(c, geom::kInnerX, y, geom::kInnerW, m.network.c_str(), style::kBodyMuted);
    y = static_cast<int16_t>(y + lineHeight(style::kBodyMuted.scale) + 12);
  }

  hline(c, geom::kInnerX, y, geom::kInnerW, kDividerGray);
  y = static_cast<int16_t>(y + 16);
  const Str lanSteps[3] = {Str::OnbStep1, Str::OnbStep2, Str::OnbStep3};
  const Str apSteps[3] = {Str::OnbApStep1, Str::OnbApStep2, Str::OnbApStep3};
  const Str* steps = m.apMode ? apSteps : lanSteps;
  const int16_t lh = lineHeight(style::kBody.scale);
  const int16_t footerY =
      static_cast<int16_t>(geom::kContentBottom - lineHeight(style::kCaption.scale));
  for (int i = 0; i < 3; ++i) {
    const int16_t room = static_cast<int16_t>((footerY - 8 - y) / lh);
    if (room <= 0) break;
    const int16_t n = drawTextWrapped(c, geom::kInnerX, y, geom::kInnerW, tr(steps[i]),
                                      style::kBody.scale, lh, std::min<int16_t>(2, room),
                                      style::kBody.gray);
    y = static_cast<int16_t>(y + std::max<int16_t>(n, 1) * lh + 10);
  }

  if (!m.reason.empty() && y + lineHeight(style::kSmall.scale) <= footerY - 8) {
    char reason[160];
    snprintf(reason, sizeof(reason), tr(Str::OnbInvalidFmt), m.reason.c_str());
    const int16_t stripH = static_cast<int16_t>(fontH(style::kCaptionStrong.scale) + 8);
    c.fillRoundRectGray(geom::kInnerX, y, geom::kInnerW, stripH, 4, 5);
    TextStyle st = style::kCaptionStrong;
    st.gray = 15;
    textFit(c, static_cast<int16_t>(geom::kInnerX + 8), centerY(y, stripH, st.scale),
            static_cast<int16_t>(geom::kInnerW - 16), reason, st);
  }

  textFit(c, geom::kInnerX, footerY, geom::kInnerW, tr(Str::OnbKeys), style::kCaption);
}

// ── 配网页 ──────────────────────────────────────────────────────────────────

void drawProvision(PortraitCanvas& c, const ProvisionModel& m) {
  if (m.succeeded) {
    textCentered(c, geom::kInnerX, geom::kInnerW,
                 centerY(geom::kBodyTop, static_cast<int16_t>(geom::kContentBottom - geom::kBodyTop),
                         style::kTitle.scale),
                 tr(Str::ProvDone), style::kTitle);
    return;
  }
  drawPageTitle(c, Str::ProvTitle);
  c.drawLine(geom::kInnerX, static_cast<int16_t>(geom::kBodyTop - 2),
             static_cast<int16_t>(geom::kInnerRight - 1), static_cast<int16_t>(geom::kBodyTop - 2),
             0);

  const int16_t lh = lineHeight(style::kBody.scale);
  int16_t y = static_cast<int16_t>(geom::kBodyTop + 16);
  const Str steps[3] = {Str::ProvStep1, Str::ProvStep2, Str::ProvStep3};
  for (Str s : steps) {
    const int16_t n = drawTextWrapped(c, geom::kInnerX, y, geom::kInnerW, tr(s),
                                      style::kBody.scale, lh, 2, style::kBody.gray);
    y = static_cast<int16_t>(y + std::max<int16_t>(n, 1) * lh + 16);
  }
  TextStyle win = style::kBodyStrong;
  win.gray = 6;
  textFit(c, geom::kInnerX, static_cast<int16_t>(y + 8), geom::kInnerW, m.windowText, win);
}

}  // namespace panel
