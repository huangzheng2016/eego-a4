// SPDX-License-Identifier: GPL-3.0-or-later
// S0–S4 与状态栏/软键栏绘制实现（见 panel_render.h 布局与边界契约）。
#include "panel_render.h"

#include <cstdio>
#include <cstring>

namespace panel {

namespace {

using eego::FontManager;
using eego::PortraitCanvas;

int16_t textW(const char* s, int16_t scale) {
  return static_cast<int16_t>(FontManager::instance().uiTextWidth(s) * scale);
}

int16_t fontH(int16_t scale) {
  return static_cast<int16_t>(FontManager::instance().uiFontHeight() * scale);
}

void textGray(PortraitCanvas& c, int16_t x, int16_t y, const char* s, int16_t scale,
              uint8_t gray, bool bold = false) {
  if (bold) {
    // 粗体 = 同位置叠加一次偏移 1px 的绘制（灰度文字用同色叠加）
    FontManager::instance().drawTextGray(c, x, y, s, scale, gray);
    FontManager::instance().drawTextGray(c, x + 1, y, s, scale, gray);
  } else {
    FontManager::instance().drawTextGray(c, x, y, s, scale, gray);
  }
}

void textRightGray(PortraitCanvas& c, int16_t x, int16_t y, int16_t w, const char* s,
                   int16_t scale, uint8_t gray, bool bold = false) {
  int16_t tw = textW(s, scale);
  int16_t ox = w - tw;
  if (ox < 0) ox = 0;
  textGray(c, static_cast<int16_t>(x + ox), y, s, scale, gray, bold);
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

// 页面标题（内容区顶部）。
void drawPageTitle(PortraitCanvas& c, Str title) {
  textGray(c, geom::kContentX + 8, geom::kContentTop + 28, tr(title), 2, 0, true);
}

//  severity 芯片（圆角灰底 + 短标签/图标字）。
void drawSevChip(PortraitCanvas& c, int16_t x, int16_t y, int16_t size, uint8_t sev) {
  c.fillRoundRectGray(x, y, size, size, 8, severityFillGray(sev));
  const int16_t s = size >= 48 ? 2 : 1;
  const int16_t tw = textW(sevShort(sev), s);
  const int16_t th = fontH(s);
  textGray(c, static_cast<int16_t>(x + (size - tw) / 2),
           static_cast<int16_t>(y + (size - th) / 2), sevShort(sev), s,
           severityTextGray(sev), true);
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

int16_t drawTextFit(PortraitCanvas& c, int16_t x, int16_t y, int16_t maxW,
                    const char* text, int16_t scale, bool bold, uint8_t gray) {
  if (text == nullptr || *text == '\0' || maxW <= 0) return 0;
  std::string s(text);
  if (textW(s.c_str(), scale) <= maxW) {
    textGray(c, x, y, s.c_str(), scale, gray, bold);
    return textW(s.c_str(), scale);
  }
  // 逐个字符回退直到 "..." 放得下（n≤120，O(n²) 可接受）
  while (!s.empty()) {
    std::string t = s + "...";
    if (textW(t.c_str(), scale) <= maxW) {
      textGray(c, x, y, t.c_str(), scale, gray, bold);
      return textW(t.c_str(), scale);
    }
    // 去最后一个 UTF-8 字符
    size_t cut = s.size() - 1;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) cut--;
    s.erase(cut);
  }
  textGray(c, x, y, "...", scale, gray, bold);
  return textW("...", scale);
}

int16_t drawTextWrapped(PortraitCanvas& c, int16_t x, int16_t y, int16_t maxW,
                        const char* text, int16_t scale, int16_t lineH, int16_t maxLines,
                        uint8_t gray) {
  if (text == nullptr || *text == '\0' || maxLines <= 0) return 0;
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
      if (textW(probe.c_str(), scale) > maxW) break;
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
      while (!cur.empty() && textW((cur + "...").c_str(), scale) > maxW) {
        size_t cut = cur.size() - 1;
        while (cut > 0 && (static_cast<unsigned char>(cur[cut]) & 0xC0) == 0x80) cut--;
        cur.erase(cut);
      }
      cur += "...";
    }
    textGray(c, x, static_cast<int16_t>(y + line * lineH), cur.c_str(), scale, gray);
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

void drawPowerStatusIcon(PortraitCanvas& c, int16_t x, int16_t y, bool mains, bool charging) {
  if (mains) {
    c.drawRect(x, static_cast<int16_t>(y + 4), 10, 12, 0);
    c.drawLine(x + 2, y, x + 2, y + 4, 0);
    c.drawLine(x + 8, y, x + 8, y + 4, 0);
    c.drawLine(x + 5, y + 12, x + 5, y + 18, 0);
    if (charging) c.fillRect(x + 2, y + 6, 6, 6, 0);
  } else {
    c.drawRect(x, static_cast<int16_t>(y + 4), 14, 16, 0);
    c.fillRect(x + 14, y + 9, 2, 6, 0);
    if (charging) c.fillRect(x + 2, y + 6, 10, 12, 0);
  }
}

// ── 状态栏 ──────────────────────────────────────────────────────────────────

void drawStatusBar(PortraitCanvas& c, const StatusBarModel& m) {
  const int16_t x = geom::kContentX;
  const int16_t y = geom::kContentY;
  const int16_t w = geom::kContentW;
  c.fillRect(x, y, w, geom::kStatusBarH, 1);  // 白底
  c.drawLine(x, static_cast<int16_t>(y + geom::kStatusBarH - 1),
             static_cast<int16_t>(x + w - 1), static_cast<int16_t>(y + geom::kStatusBarH - 1),
             0);

  // 左：时间（RTC 有效时 MM-DD HH:MM，否则 --:--）
  textGray(c, x + 8, static_cast<int16_t>(y + 6), m.timeText, 2, 0, true);

  // 中：WiFi 状态居中；电源模式以小图标显示在电量旁，避免遮挡时间
  const char* wifi = tr(Str::WifiDisconnected);
  switch (m.wifiState) {
    case 1: wifi = tr(Str::WifiConnecting); break;
    case 2: wifi = tr(Str::WifiConnected); break;
    case 3: wifi = tr(Str::WifiFailed); break;
    case 4: wifi = tr(Str::WifiOffline); break;
    default: break;
  }
  const int16_t wifiW = textW(wifi, 2);
  const int16_t minCx = x + 130;
  const int16_t maxCx = static_cast<int16_t>(x + w - 130 - wifiW);
  int16_t cx = static_cast<int16_t>(x + (w - wifiW) / 2);
  if (maxCx < minCx) {
    cx = minCx;
  } else {
    if (cx < minCx) cx = minCx;
    if (cx > maxCx) cx = maxCx;
  }
  const int16_t wifiMaxW = static_cast<int16_t>(x + w - 124 - cx);
  if (wifiMaxW > 40) {
    cx = static_cast<int16_t>(cx + drawTextFit(c, cx, static_cast<int16_t>(y + 6), wifiMaxW,
                                               wifi, 2, true, 6) +
                              10);
  }
  if ((m.portalOpen || m.apFallback) && cx < x + w - 200) {
    const char* badge = m.portalOpen ? tr(Str::PortalOpen) : tr(Str::ApFallback);
    textGray(c, cx, static_cast<int16_t>(y + 6), badge, 1, 0, true);
    cx = static_cast<int16_t>(cx + textW(badge, 1) + 10);
  }
  if (m.mutedCount > 0 && cx < x + w - 110) {
    char mute[16];
    snprintf(mute, sizeof(mute), "%s%u", tr(Str::Mute), static_cast<unsigned>(m.mutedCount));
    textGray(c, cx, static_cast<int16_t>(y + 6), mute, 1, 6);
  }

  // 右：供电图标 + 电池百分比
  drawPowerStatusIcon(c, static_cast<int16_t>(x + w - 112), static_cast<int16_t>(y + 8),
                      m.mains, m.charging);
  char bat[24];
  snprintf(bat, sizeof(bat), "%u%%%s", static_cast<unsigned>(m.batPercent),
           m.charging ? "+" : "");
  textRightGray(c, static_cast<int16_t>(x + w - 88), static_cast<int16_t>(y + 6), 80, bat, 2,
                0, true);

  // 横幅（操作结果/二次确认）：内容区顶部 28px 浅灰条
  if (m.banner[0] != '\0') {
    c.fillRect(x, geom::kContentTop, w, 28, 1);
    c.fillRoundRectGray(x, geom::kContentTop, w, 28, 4, 11);
    textGray(c, static_cast<int16_t>(x + 10), static_cast<int16_t>(geom::kContentTop + 7),
             m.banner, 1, 0, true);
  }
}

// ── 软键栏 ──────────────────────────────────────────────────────────────────

void drawSoftKeys(PortraitCanvas& c, const SoftKeyModel* keys, size_t n, size_t focused) {
  if (keys == nullptr || n == 0) return;
  const int16_t gap = 8;
  const int16_t totalW = geom::kContentW;
  const int16_t kw = static_cast<int16_t>((totalW - gap * (n - 1)) / n);
  for (size_t i = 0; i < n; i++) {
    const int16_t x = static_cast<int16_t>(geom::kContentX + i * (kw + gap));
    const bool sel = (i == focused);
    int16_t scale = 3;
    if (textW(keys[i].label, 3) > kw - 12 || fontH(3) > geom::kSoftKeyH - 8) scale = 2;
    if (textW(keys[i].label, scale) > kw - 12 || fontH(scale) > geom::kSoftKeyH - 8) scale = 1;
    const int16_t tw = textW(keys[i].label, scale);
    const int16_t tx = static_cast<int16_t>(x + (kw - tw) / 2);
    const int16_t ty = static_cast<int16_t>(geom::kSoftKeyY + (geom::kSoftKeyH - fontH(scale)) / 2);
    const int16_t textX = tx < x + 6 ? static_cast<int16_t>(x + 6) : tx;
    if (!keys[i].enabled) {
      // 禁用：浅灰底 + 中灰字（不可用 BW 描边，白边在白底上不可见）
      c.fillRoundRectGray(x, geom::kSoftKeyY, kw, geom::kSoftKeyH, 8, 12);
      drawTextFit(c, textX, ty, kw - 12, keys[i].label, scale, false, 6);
    } else if (sel) {
      c.fillRoundRectGray(x, geom::kSoftKeyY, kw, geom::kSoftKeyH, 8, 0);
      drawTextFit(c, textX, ty, kw - 12, keys[i].label, scale, true, 15);
    } else {
      c.drawRoundRect(x, geom::kSoftKeyY, kw, geom::kSoftKeyH, 8, 0);
      drawTextFit(c, textX, ty, kw - 12, keys[i].label, scale, false, 0);
    }
  }
}

// ── S0 总览 ─────────────────────────────────────────────────────────────────

void drawS0(PortraitCanvas& c, const S0Model& m) {
  drawPageTitle(c, Str::PageOverview);
  const int16_t x = geom::kContentX;
  const int16_t w = geom::kContentW;

  if (m.empty) {
    // 空态：浅灰圆环 + 居中提示
    c.drawCircle(static_cast<int16_t>(x + w / 2), 300, 60, 0);
    c.drawCircle(static_cast<int16_t>(x + w / 2), 300, 44, 0);
    const char* msg = tr(Str::NoAlerts);
    const int16_t tw = textW(msg, 3);
    textGray(c, static_cast<int16_t>(x + (w - tw) / 2), 390, msg, 3, 0, true);
  } else {
    struct Card {
      size_t count;
      Str label;
      uint8_t sev;
    };
    const Card cards[3] = {{m.critical, Str::SevCritical, 2},
                           {m.warning, Str::SevWarning, 1},
                           {m.info, Str::SevInfo, 0}};
    int16_t y = static_cast<int16_t>(geom::kContentTop + 64);
    for (const Card& card : cards) {
      c.fillRoundRectGray(x, y, w, 130, 12, severityFillGray(card.sev));
      textGray(c, static_cast<int16_t>(x + 28), static_cast<int16_t>(y + 34),
               tr(card.label), 3, severityTextGray(card.sev), true);
      char cnt[16];
      snprintf(cnt, sizeof(cnt), "%u", static_cast<unsigned>(card.count));
      textRightGray(c, static_cast<int16_t>(x + w - 228), static_cast<int16_t>(y + 22), 200,
                    cnt, 6, severityTextGray(card.sev), true);
      y = static_cast<int16_t>(y + 130 + 10);
    }
    char line[96];
    snprintf(line, sizeof(line), "%s %u · %s %u · %s %u", tr(Str::ActiveTotal),
             static_cast<unsigned>(m.activeTotal), tr(Str::AckedCount),
             static_cast<unsigned>(m.acked), tr(Str::MutedCount),
             static_cast<unsigned>(m.muted));
    textGray(c, static_cast<int16_t>(x + 8), static_cast<int16_t>(y + 12), line, 1, 0, true);
    snprintf(line, sizeof(line), "%s %u/%u", tr(Str::SourcesHealthy),
             static_cast<unsigned>(m.sourcesOk), static_cast<unsigned>(m.sourcesTotal));
    textGray(c, static_cast<int16_t>(x + 8), static_cast<int16_t>(y + 40), line, 1, 6);
  }

  // 更新时间（空态/非空态都画在底部）
  if (m.updatedText[0] != '\0') {
    char upd[48];
    snprintf(upd, sizeof(upd), "%s %s", tr(Str::UpdatedAt), m.updatedText);
    textGray(c, static_cast<int16_t>(x + 8), static_cast<int16_t>(geom::kContentBottom - 22),
             upd, 1, 6);
  }
}

// ── S1 表格 ─────────────────────────────────────────────────────────────────

void drawS1(PortraitCanvas& c, const S1Model& m) {
  const int16_t x = geom::kContentX;
  const int16_t w = geom::kContentW;
  char head[48];
  snprintf(head, sizeof(head), tr(Str::AlertsCountFmt), static_cast<unsigned>(m.total));
  textGray(c, static_cast<int16_t>(x + 8), static_cast<int16_t>(geom::kContentTop + 28), head,
           2, 0, true);
  char pg[32];
  snprintf(pg, sizeof(pg), tr(Str::PageFmt), static_cast<unsigned>(m.page + 1),
           static_cast<unsigned>(m.pageCount));
  textRightGray(c, static_cast<int16_t>(x + w - 168), static_cast<int16_t>(geom::kContentTop + 34),
                160, pg, 1, 6);

  if (m.rows.empty()) {
    const int16_t tw = textW(tr(Str::EmptyList), 2);
    textGray(c, static_cast<int16_t>(x + (w - tw) / 2), 320, tr(Str::EmptyList), 2, 6, true);
    return;
  }

  int16_t y = static_cast<int16_t>(geom::kContentTop + 60);
  for (size_t i = 0; i < m.rows.size() && i < static_cast<size_t>(geom::kRowsPerPage); i++) {
    const S1RowModel& row = m.rows[i];
    const bool sel = (i == m.selected);
    if (sel) {
      c.fillRoundRectGray(static_cast<int16_t>(x + 2), static_cast<int16_t>(y - 4),
                          static_cast<int16_t>(w - 4), static_cast<int16_t>(geom::kRowH - 6),
                          8, 11);
      c.drawRoundRect(static_cast<int16_t>(x + 2), static_cast<int16_t>(y - 4),
                      static_cast<int16_t>(w - 4), static_cast<int16_t>(geom::kRowH - 6), 8,
                      0);
    }
    drawSevChip(c, static_cast<int16_t>(x + 14), static_cast<int16_t>(y + 12), 56,
                row.severity);
    drawTextFit(c, static_cast<int16_t>(x + 86), static_cast<int16_t>(y + 8),
                static_cast<int16_t>(w - 230), row.title.c_str(), 2, true, 0);
    drawTextFit(c, static_cast<int16_t>(x + 86), static_cast<int16_t>(y + 42),
                static_cast<int16_t>(w - 230), row.subtitle.c_str(), 1, false, 6);
    // 右侧：状态 + 相对时间（右缘对齐内容区 -16）
    textRightGray(c, static_cast<int16_t>(x + w - 136), static_cast<int16_t>(y + 8), 120,
                  stateLabel(row.state), 1, row.state == 0 ? 0 : 6, row.state == 0);
    textRightGray(c, static_cast<int16_t>(x + w - 136), static_cast<int16_t>(y + 42), 120,
                  row.age, 1, 6);
    if (!sel) {
      c.drawLine(static_cast<int16_t>(x + 8), static_cast<int16_t>(y + geom::kRowH - 8),
                 static_cast<int16_t>(x + w - 8), static_cast<int16_t>(y + geom::kRowH - 8),
                 1);
    }
    y = static_cast<int16_t>(y + geom::kRowH);
  }
}

// ── S2 详情 ─────────────────────────────────────────────────────────────────

void drawS2(PortraitCanvas& c, const S2Model& m) {
  const int16_t x = geom::kContentX;
  const int16_t w = geom::kContentW;
  int16_t y = static_cast<int16_t>(geom::kContentTop + 32);

  drawSevChip(c, static_cast<int16_t>(x + 8), y, 40, m.severity);
  drawTextFit(c, static_cast<int16_t>(x + 60), static_cast<int16_t>(y + 8),
                static_cast<int16_t>(w - 180), m.typeName.c_str(), 2, true, 0);
  textRightGray(c, static_cast<int16_t>(x + w - 122), static_cast<int16_t>(y + 12), 110,
                stateLabel(m.state), 1, 6);
  y = static_cast<int16_t>(y + 52);

  // 标题（粗体，最多 2 行）
  y = static_cast<int16_t>(y + drawTextWrapped(c, static_cast<int16_t>(x + 8), y,
                                               static_cast<int16_t>(w - 16), m.title.c_str(),
                                               2, 26, 2, 0) *
                                26 +
                            6);
  // 消息（最多 5 行）
  y = static_cast<int16_t>(y + drawTextWrapped(c, static_cast<int16_t>(x + 8), y,
                                               static_cast<int16_t>(w - 16),
                                               m.message.c_str(), 1, 18, 5, 0) *
                                18 +
                            8);

  // 元信息两列（最多 8 项；标签列宽 60px，值列紧随）
  const int16_t colW = static_cast<int16_t>((w - 16) / 2);
  size_t i = 0;
  for (; i < m.meta.size() && i < 8; i++) {
    const int16_t cx = static_cast<int16_t>(x + 8 + (i % 2) * colW);
    const int16_t cy = static_cast<int16_t>(y + (i / 2) * 22);
    drawTextFit(c, cx, cy, 60, m.meta[i].first.c_str(), 1, false, 6);
    drawTextFit(c, static_cast<int16_t>(cx + 66), cy, static_cast<int16_t>(colW - 74),
                m.meta[i].second.c_str(), 1, false, 0);
  }
  if (!m.meta.empty()) {
    y = static_cast<int16_t>(y + ((i + 1) / 2) * 22 + 6);
  }

  // 字段表（最多 4 行，浅灰底条）
  for (size_t f = 0; f < m.fields.size() && f < 4; f++) {
    const int16_t fy = static_cast<int16_t>(y + f * 22);
    if (fy + 20 > geom::kContentBottom) break;
    c.fillRoundRectGray(static_cast<int16_t>(x + 8), fy, static_cast<int16_t>(w - 16), 20, 4,
                        12);
    drawTextFit(c, static_cast<int16_t>(x + 16), static_cast<int16_t>(fy + 3), 150,
                m.fields[f].first.c_str(), 1, true, 6);
    drawTextFit(c, static_cast<int16_t>(x + 172), static_cast<int16_t>(fy + 3),
                static_cast<int16_t>(w - 190), m.fields[f].second.c_str(), 1, false, 0);
  }
}

// ── S3 状态 ─────────────────────────────────────────────────────────────────

void drawS3(PortraitCanvas& c, const S3Model& m) {
  drawPageTitle(c, Str::PageStatus);
  const int16_t x = geom::kContentX;
  const int16_t w = geom::kContentW;
  int16_t y = static_cast<int16_t>(geom::kContentTop + 64);
  for (const auto& row : m.rows) {
    if (y + 20 > geom::kContentBottom) break;
    if (row.second.empty()) {
      // 分节标题
      textGray(c, static_cast<int16_t>(x + 8), y, row.first.c_str(), 1, 0, true);
      c.drawLine(static_cast<int16_t>(x + 8), static_cast<int16_t>(y + 18),
                 static_cast<int16_t>(x + w - 8), static_cast<int16_t>(y + 18), 1);
      y = static_cast<int16_t>(y + 26);
    } else {
      drawTextFit(c, static_cast<int16_t>(x + 16), y, 110, row.first.c_str(), 1, false, 6);
      drawTextFit(c, static_cast<int16_t>(x + 132), y, static_cast<int16_t>(w - 144),
                  row.second.c_str(), 1, false, 0);
      y = static_cast<int16_t>(y + 22);
    }
  }
  if (!m.configValid && y + 30 < geom::kContentBottom) {
    c.fillRoundRectGray(static_cast<int16_t>(x + 8), static_cast<int16_t>(y + 4),
                        static_cast<int16_t>(w - 16), 24, 4, 5);
    textGray(c, static_cast<int16_t>(x + 16), static_cast<int16_t>(y + 9),
             tr(Str::ConfigBad), 1, 15, true);
  }
}

void drawHelp(PortraitCanvas& c, const S3Model& m) {
  drawPageTitle(c, Str::PageHelp);
  const int16_t x = geom::kContentX;
  const int16_t w = geom::kContentW;
  int16_t y = static_cast<int16_t>(geom::kContentTop + 58);
  for (const auto& row : m.rows) {
    if (y + 20 > geom::kContentBottom) break;
    if (row.second.empty()) {
      textGray(c, static_cast<int16_t>(x + 8), y, row.first.c_str(), 1, 0, true);
      y = static_cast<int16_t>(y + 26);
    } else {
      drawTextFit(c, static_cast<int16_t>(x + 12), y, 142, row.first.c_str(), 1, false, 6);
      drawTextFit(c, static_cast<int16_t>(x + 158), y, static_cast<int16_t>(w - 170),
                  row.second.c_str(), 1, false, 0);
      y = static_cast<int16_t>(y + 24);
    }
  }
}

// ── S4 设置 ─────────────────────────────────────────────────────────────────

void drawS4(PortraitCanvas& c, const S4Model& m) {
  drawPageTitle(c, Str::PageSettings);
  char page[32];
  snprintf(page, sizeof(page), tr(Str::SettingsPageFmt), static_cast<unsigned>(m.page + 1),
           static_cast<unsigned>(m.pageCount));
  textRightGray(c, geom::kContentX + geom::kContentW - 160, geom::kContentTop + 34, 152,
                page, 1, 6);
  const int16_t x = geom::kContentX;
  const int16_t w = geom::kContentW;
  int16_t y = static_cast<int16_t>(geom::kContentTop + 64);
  for (size_t i = 0; i < m.rows.size(); i++) {
    const S4RowModel& row = m.rows[i];
    const bool sel = (i == m.selected);
    if (sel) {
      c.fillRoundRectGray(static_cast<int16_t>(x + 2), static_cast<int16_t>(y - 4),
                          static_cast<int16_t>(w - 4), 52, 8, 11);
      c.drawRoundRect(static_cast<int16_t>(x + 2), static_cast<int16_t>(y - 4),
                      static_cast<int16_t>(w - 4), 52, 8, 0);
    }
    drawTextFit(c, static_cast<int16_t>(x + 16), static_cast<int16_t>(y + 12),
                static_cast<int16_t>(w / 2), row.label.c_str(), 2, true, 0);
    drawTextFit(c, static_cast<int16_t>(x + w / 2 + 40), static_cast<int16_t>(y + 14),
                static_cast<int16_t>(w / 2 - 96), row.value.c_str(), 1, false,
                row.isAction ? 0 : 6);
    if (!row.isAction) {
      // 开关/查看行右侧 chevron
      c.drawIcon(static_cast<int16_t>(x + w - 36), static_cast<int16_t>(y + 16),
                 eego::ICON_CHEVRON_RIGHT_16, 16, 16, 0);
    }
    y = static_cast<int16_t>(y + 56);
    if (y + 48 > geom::kContentBottom) break;
  }
}

// ── 配网页 ──────────────────────────────────────────────────────────────────

void drawProvision(PortraitCanvas& c, const ProvisionModel& m) {
  const int16_t x = geom::kContentX;
  const int16_t w = geom::kContentW;
  if (m.succeeded) {
    const char* msg = tr(Str::ProvDone);
    const int16_t tw = textW(msg, 2);
    textGray(c, static_cast<int16_t>(x + (w - tw) / 2), 340, msg, 2, 0, true);
    return;
  }
  textGray(c, static_cast<int16_t>(x + 24), 150, tr(Str::ProvTitle), 3, 0, true);
  c.drawLine(static_cast<int16_t>(x + 24), 200, static_cast<int16_t>(x + w - 24), 200, 0);

  int16_t y = 240;
  const Str steps[3] = {Str::ProvStep1, Str::ProvStep2, Str::ProvStep3};
  for (Str s : steps) {
    drawTextWrapped(c, static_cast<int16_t>(x + 24), y, static_cast<int16_t>(w - 48),
                    tr(s), 2, 30, 2, 0);
    y = static_cast<int16_t>(y + 76);
  }
  textGray(c, static_cast<int16_t>(x + 24), static_cast<int16_t>(y + 20),
           m.windowText, 2, 6, true);
}

}  // namespace panel
