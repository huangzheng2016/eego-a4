// SPDX-License-Identifier: GPL-3.0-or-later
#include "panel_render.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace panel {
namespace {
using eego::FontManager;
using eego::PortraitCanvas;

constexpr uint8_t kDividerGray = 9;
constexpr uint8_t kRuleGray = 6;
constexpr int16_t kKvLabelW = 152;   // alert-detail meta column
constexpr int16_t kKvLabelMin = 96;
constexpr int16_t kKvLabelMax = 228;
constexpr int16_t kKvValueLines = 2;
constexpr int16_t kFieldLabelW = 150;

int16_t clampScale(int16_t scale) { return std::max<int16_t>(scale, kMinTextScale); }

int16_t textW(const char* s, int16_t scale) {
  return static_cast<int16_t>(FontManager::instance().uiTextWidth(s) * scale);
}

int16_t fontH(int16_t scale) {
  return static_cast<int16_t>(FontManager::instance().uiFontHeight() * scale);
}

int16_t boldExtra(bool bold, int16_t scale) {
  return bold ? std::max<int16_t>(1, scale) : 0;
}

int16_t styleW(const char* s, const TextStyle& st) {
  return static_cast<int16_t>(textW(s, st.scale) + boldExtra(st.bold, st.scale));
}

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

void textRight(PortraitCanvas& c, int16_t right, int16_t y, int16_t maxW, const char* s,
               const TextStyle& st) {
  const int16_t tw = std::min(styleW(s, st), maxW);
  textFit(c, static_cast<int16_t>(right - tw), y, maxW, s, st);
}

void textCentered(PortraitCanvas& c, int16_t x, int16_t w, int16_t y, const char* s,
                  const TextStyle& st) {
  const int16_t tw = std::min(styleW(s, st), w);
  textFit(c, static_cast<int16_t>(x + (w - tw) / 2), y, w, s, st);
}

void hline(PortraitCanvas& c, int16_t x, int16_t y, int16_t w, uint8_t gray) {
  c.fillRoundRectGray(x, y, w, 1, 0, gray);
}

size_t utf8CharLen(unsigned char lead) {
  if (lead < 0x80) return 1;
  if ((lead & 0xE0) == 0xC0) return 2;
  if ((lead & 0xF0) == 0xE0) return 3;
  if ((lead & 0xF8) == 0xF0) return 4;
  return 1;
}

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

void drawTitleText(PortraitCanvas& c, const char* title, int16_t maxW) {
  textFit(c, geom::kInnerX, centerY(geom::kTitleY, geom::kTitleH, style::kTitle.scale),
          maxW, title, style::kTitle);
}

void drawPageTitle(PortraitCanvas& c, Str title) {
  drawTitleText(c, tr(title), geom::kInnerW);
}

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

constexpr float kBoltX[7] = {0.62f, 0.02f, 0.44f, 0.26f, 0.98f, 0.56f, 0.90f};
constexpr float kBoltY[7] = {0.00f, 0.58f, 0.58f, 1.00f, 0.40f, 0.40f, 0.00f};

int16_t batteryIconWidth(int16_t h) { return static_cast<int16_t>(h * 2 + 3); }

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

int16_t plugIconWidth(int16_t h) { return static_cast<int16_t>(std::max<int16_t>(8, h * 3 / 5)); }

void drawPlugIcon(PortraitCanvas& c, int16_t x, int16_t y, int16_t h) {
  const int16_t w = plugIconWidth(h);
  const int16_t pinH = static_cast<int16_t>(h / 4);
  const int16_t bodyH = static_cast<int16_t>(h * 2 / 5);
  const int16_t cordH = static_cast<int16_t>(h - pinH - bodyH);
  const int16_t pinW = 2;
  c.fillRect(static_cast<int16_t>(x + w / 4 - pinW / 2), y, pinW, pinH, 0);
  c.fillRect(static_cast<int16_t>(x + w - w / 4 - pinW / 2), y, pinW, pinH, 0);
  c.fillRoundRectGray(x, static_cast<int16_t>(y + pinH), w, bodyH,
                      static_cast<int16_t>(std::min<int16_t>(3, w / 4)), 0);
  c.fillRect(static_cast<int16_t>(x + w / 2 - 1), static_cast<int16_t>(y + pinH + bodyH), 2,
             cordH, 0);
}

int16_t wifiIconWidth(int16_t h) { return static_cast<int16_t>(h * 13 / 10 + 1); }

bool wifiBandHit(float dx, float dy, float h) {
  if (dy <= 0.0f) return false;
  const float r2 = dx * dx + dy * dy;
  const float dot = 0.20f * h;
  if (r2 <= dot * dot) return true;
  const float ax = dx < 0.0f ? -dx : dx;
  if (ax > dy) return false;
  const float a0 = 0.40f * h, a1 = 0.60f * h;
  const float b0 = 0.76f * h, b1 = 0.97f * h;
  return (r2 >= a0 * a0 && r2 <= a1 * a1) || (r2 >= b0 * b0 && r2 <= b1 * b1);
}

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
    const int16_t x1 = static_cast<int16_t>(x + w - 1);
    const int16_t y1 = static_cast<int16_t>(y + h - 1);
    c.drawLine(static_cast<int16_t>(x - 1), y, static_cast<int16_t>(x1 - 1), y1, 1);
    c.drawLine(static_cast<int16_t>(x + 2), y, static_cast<int16_t>(x1 + 2), y1, 1);
    c.drawLine(x, y, x1, y1, 0);
    c.drawLine(static_cast<int16_t>(x + 1), y, static_cast<int16_t>(x1 + 1), y1, 0);
  }
  return w;
}

int16_t kvHeadingH() { return static_cast<int16_t>(fontH(style::kBodyStrong.scale) + 12); }

int16_t kvValueW(int16_t labelW = kKvLabelW) {
  return static_cast<int16_t>(geom::kInnerW - labelW);
}

int16_t drawKvList(PortraitCanvas& c, const std::vector<std::pair<std::string, std::string>>& rows,
                   int16_t y) {
  const int16_t labelW = kvLabelWidth(rows);
  const int16_t lh = lineHeight(style::kBody.scale);
  const int16_t fh = fontH(style::kBody.scale);
  for (const auto& row : rows) {
    if (row.second.empty()) {
      const int16_t fhBig = fontH(style::kBodyStrong.scale);
      if (y + fhBig > geom::kContentBottom) break;
      textFit(c, geom::kInnerX, y, geom::kInnerW, row.first.c_str(), style::kBodyStrong);
      hline(c, geom::kInnerX, static_cast<int16_t>(y + fhBig + 3), geom::kInnerW, kRuleGray);
      y = static_cast<int16_t>(y + kvHeadingH());
      continue;
    }
    if (y + fh > geom::kContentBottom) break;
    const int16_t room = static_cast<int16_t>((geom::kContentBottom - y - fh) / lh + 1);
    textFit(c, geom::kInnerX, y, static_cast<int16_t>(labelW - 12), row.first.c_str(),
            style::kBodyMuted);
    const int16_t lines = drawTextWrapped(
        c, static_cast<int16_t>(geom::kInnerX + labelW), y, kvValueW(labelW), row.second.c_str(),
        style::kBody.scale, lh, std::min<int16_t>(kKvValueLines, room), style::kBody.gray);
    y = static_cast<int16_t>(y + std::max<int16_t>(1, lines) * lh);
  }
  return y;
}

}  // namespace

void sanitizeClamped(std::string& s, size_t maxChars) {
  std::string out;
  out.reserve(s.size());
  size_t chars = 0;
  size_t i = 0;
  while (i < s.size() && chars < maxChars) {
    const unsigned char b = static_cast<unsigned char>(s[i]);
    if (b < 0x20 || b == 0x7F) {
      i++;
      continue;
    }
    const size_t len = utf8CharLen(b);
    if (i + len > s.size()) break;
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
  scale = clampScale(scale);
  const int16_t extra = boldExtra(bold, scale);
  std::string s(text);
  if (textW(s.c_str(), scale) + extra <= maxW) {
    textGray(c, x, y, s.c_str(), scale, gray, bold);
    return static_cast<int16_t>(textW(s.c_str(), scale) + extra);
  }
  while (!s.empty()) {
    std::string t = s + "...";
    if (textW(t.c_str(), scale) + extra <= maxW) {
      textGray(c, x, y, t.c_str(), scale, gray, bold);
      return static_cast<int16_t>(textW(t.c_str(), scale) + extra);
    }
    size_t cut = s.size() - 1;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) cut--;
    s.erase(cut);
  }
  textGray(c, x, y, "...", scale, gray, bold);
  return static_cast<int16_t>(textW("...", scale) + extra);
}

namespace {
std::vector<std::string> wrapLines(const char* text, int16_t scale, int16_t fitW,
                                   int16_t maxLines) {
  std::vector<std::string> out;
  if (text == nullptr || *text == '\0' || maxLines <= 0) return out;
  const std::string s(text);
  size_t pos = 0;
  while (pos < s.size() && static_cast<int16_t>(out.size()) < maxLines) {
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
    if (end == pos) end = s.size();
    const bool lastLine = static_cast<int16_t>(out.size()) == maxLines - 1;
    std::string cur = s.substr(pos, end - pos);
    // Break at a space only when that keeps most of the line (Latin words);
    // CJK text breaks anywhere, so a space early on must not strand a stub.
    if (lastSpace != std::string::npos && lastSpace > pos && end < s.size() &&
        textW(s.substr(pos, lastSpace - pos).c_str(), scale) * 3 >= fitW * 2) {
      cur = s.substr(pos, lastSpace - pos);
      end = lastSpace + 1;
    }
    if (lastLine && end < s.size()) {
      while (!cur.empty() && textW((cur + "...").c_str(), scale) > fitW) {
        size_t cut = cur.size() - 1;
        while (cut > 0 && (static_cast<unsigned char>(cur[cut]) & 0xC0) == 0x80) cut--;
        cur.erase(cut);
      }
      cur += "...";
    }
    out.push_back(std::move(cur));
    pos = end;
  }
  return out;
}
}  // namespace

int16_t drawTextWrapped(PortraitCanvas& c, int16_t x, int16_t y, int16_t maxW,
                        const char* text, int16_t scale, int16_t lineH, int16_t maxLines,
                        uint8_t gray, bool bold) {
  scale = clampScale(scale);
  const std::vector<std::string> lines =
      wrapLines(text, scale, static_cast<int16_t>(maxW - boldExtra(bold, scale)), maxLines);
  for (size_t i = 0; i < lines.size(); ++i) {
    textGray(c, x, static_cast<int16_t>(y + static_cast<int16_t>(i) * lineH), lines[i].c_str(),
             scale, gray, bold);
  }
  return static_cast<int16_t>(lines.size());
}

int16_t kvLabelWidth(const std::vector<std::pair<std::string, std::string>>& rows) {
  int16_t w = 0;
  for (const auto& row : rows) {
    if (!row.second.empty()) w = std::max(w, styleW(row.first.c_str(), style::kBodyMuted));
  }
  return std::clamp<int16_t>(static_cast<int16_t>(w + 12), kKvLabelMin, kKvLabelMax);
}

int16_t kvRowHeight(const std::string& value, int16_t labelW) {
  if (value.empty()) return kvHeadingH();
  const size_t lines =
      wrapLines(value.c_str(), style::kBody.scale, kvValueW(labelW), kKvValueLines).size();
  return static_cast<int16_t>(std::max<size_t>(1, lines) * lineHeight(style::kBody.scale));
}

uint8_t severityFillGray(uint8_t severity) {
  switch (severity) {
    case 2: return 0;
    case 1: return 5;
    default: return 9;
  }
}

uint8_t severityTextGray(uint8_t severity) {
  return severity == 0 ? 0 : 15;
}

//

void drawStatusBar(PortraitCanvas& c, const StatusBarModel& m) {
  const int16_t x = geom::kContentX;
  const int16_t y = geom::kContentY;
  const int16_t w = geom::kContentW;
  const int16_t barH = static_cast<int16_t>(geom::kStatusBarH - 1);
  c.fillRect(x, y, w, geom::kStatusBarH, 1);
  c.drawLine(x, static_cast<int16_t>(y + barH), static_cast<int16_t>(x + w - 1),
             static_cast<int16_t>(y + barH), 0);

  const TextStyle body = style::kBody;
  TextStyle strong = body;
  strong.bold = true;
  TextStyle muted = body;
  muted.gray = 6;
  const int16_t ty = centerY(y, barH, body.scale);

  const int16_t capH = std::max<int16_t>(12, static_cast<int16_t>(fontH(body.scale) / 2));
  const int16_t leftEnd = static_cast<int16_t>(
      geom::kInnerX + drawTextFit(c, geom::kInnerX, ty, static_cast<int16_t>(w / 2 - geom::kPadX),
                                  m.timeText, strong.scale, strong.bold, strong.gray));

  // Right cluster, icons only: WiFi (crossed out unless connected), plug, battery, %.
  int16_t rx = geom::kInnerRight;
  const bool hasBattery = m.batMv > 0;
  if (hasBattery) {
    char bat[8];
    snprintf(bat, sizeof(bat), "%u%%", static_cast<unsigned>(std::min<int>(m.batPercent, 100)));
    rx = static_cast<int16_t>(rx - styleW(bat, body));
    text(c, rx, ty, bat, body);
    rx = static_cast<int16_t>(rx - 6);
  }
  const int16_t batH = static_cast<int16_t>(capH & ~1);
  if (hasBattery) {
    rx = static_cast<int16_t>(rx - batteryIconWidth(batH));
    drawBatteryIcon(c, rx, static_cast<int16_t>(y + (barH - batH) / 2), batH, m.batPercent,
                    m.charging);
  }
  if (m.mains || !hasBattery) {
    const int16_t plugH = static_cast<int16_t>(batH + 4);
    rx = static_cast<int16_t>(rx - (hasBattery ? 5 : 0) - plugIconWidth(plugH));
    drawPlugIcon(c, rx, static_cast<int16_t>(y + (barH - plugH) / 2), plugH);
  }
  rx = static_cast<int16_t>(rx - 12);
  const int16_t wifiH = static_cast<int16_t>(capH + 4);
  rx = static_cast<int16_t>(rx - wifiIconWidth(wifiH));
  drawWifiIcon(c, rx, static_cast<int16_t>(y + (barH - wifiH) / 2), wifiH, m.wifiState);

  // Middle: link pill, then the longest state label that fits, then the mute count.
  const int16_t mx1 = static_cast<int16_t>(rx - 12);
  int16_t bx = static_cast<int16_t>(leftEnd + 16);
  const char* pill = m.portalOpen || m.apFallback ? tr(Str::HotspotPill)
                     : m.agentUplink              ? tr(Str::AgentUplinkPill)
                     : m.bleLinked                ? tr(Str::BleLinkedPill)
                                                  : nullptr;
  const int16_t pillPad = 8;
  const int16_t pillW =
      pill != nullptr ? static_cast<int16_t>(styleW(pill, strong) + 2 * pillPad) : 0;
  if (pillW > 0 && bx + pillW <= mx1) {
    const int16_t pillH = std::min<int16_t>(static_cast<int16_t>(fontH(body.scale) + 4),
                                            static_cast<int16_t>(barH - 4));
    const int16_t py = static_cast<int16_t>(y + (barH - pillH) / 2);
    c.fillRoundRectGray(bx, py, pillW, pillH, static_cast<int16_t>(pillH / 2), 0);
    TextStyle onDark = strong;
    onDark.gray = 15;
    text(c, static_cast<int16_t>(bx + pillPad), ty, pill, onDark);
    bx = static_cast<int16_t>(bx + pillW + 10);
  }
  char mute[24] = "";
  if (m.mutedCount > 0) {
    snprintf(mute, sizeof(mute), "%s%u", tr(Str::Mute), static_cast<unsigned>(m.mutedCount));
  }
  const int16_t muteW = mute[0] != '\0' ? styleW(mute, muted) : 0;
  if (!m.stateLabels.empty()) {
    const int16_t icon = stateIconSize(fontH(body.scale));
    const int16_t room = static_cast<int16_t>(mx1 - bx - icon - 6);
    const std::string* label = &m.stateLabels.back();
    for (const std::string& l : m.stateLabels) {
      if (styleW(l.c_str(), body) <= room) {
        label = &l;
        break;
      }
    }
    const int16_t stateW = std::min(styleW(label->c_str(), body), room);
    if (room >= styleW("...", body) + textW("W", body.scale)) {
      drawStateIcon(c, bx, static_cast<int16_t>(y + (barH - icon) / 2), icon, m.state);
      textFit(c, static_cast<int16_t>(bx + icon + 6), ty, room, label->c_str(), body);
      bx = static_cast<int16_t>(bx + icon + 6 + stateW + 10);
    }
  }
  if (muteW > 0 && bx + muteW <= mx1) text(c, bx, ty, mute, muted);
}

void drawBanner(PortraitCanvas& c, const StatusBarModel& m) {
  if (m.banner[0] == '\0') return;
  const int16_t x = geom::kContentX;
  const int16_t y = geom::kContentY;
  const int16_t w = geom::kContentW;
  const int16_t barH = static_cast<int16_t>(geom::kStatusBarH - 1);
  const TextStyle strong = style::kBodyStrong;
  const int16_t ty = centerY(y, barH, strong.scale);
  // A transient banner replaces the whole bar for its few seconds; a long one
  // grows to two lines over the top of the page instead of shrinking.
  const int16_t lh = lineHeight(strong.scale);
  const int16_t textW2 = static_cast<int16_t>(geom::kInnerW - 4);
  const bool two = styleW(m.banner, strong) > textW2;
  const int16_t boxH = two ? static_cast<int16_t>(barH - 4 + lh) : static_cast<int16_t>(barH - 4);
  c.fillRect(x, y, w, static_cast<int16_t>(boxH + 4), 1);
  c.fillRoundRectGray(static_cast<int16_t>(x + 2), static_cast<int16_t>(y + 2),
                      static_cast<int16_t>(w - 4), boxH, 6, 11);
  drawTextWrapped(c, geom::kInnerX, ty, textW2, m.banner, strong.scale, lh, 2, strong.gray,
                  strong.bold);
}

void drawSoftKeys(PortraitCanvas& c, const SoftKeyModel* keys, size_t n, size_t focused) {
  if (keys == nullptr || n == 0) return;
  const int16_t gap = geom::kSoftKeyGap;
  const int16_t totalW = geom::kContentW;
  const int16_t kw = static_cast<int16_t>((totalW - gap * static_cast<int16_t>(n - 1)) /
                                          static_cast<int16_t>(n));
  const int16_t inner = static_cast<int16_t>(kw - 12);
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

  const int16_t lh2 = lineHeight(style::kBodyStrong.scale);
  const int16_t titleLines = drawTextWrapped(c, geom::kInnerX, y, geom::kInnerW, m.title.c_str(),
                                             style::kBodyStrong.scale, lh2, 2,
                                             style::kBodyStrong.gray, style::kBodyStrong.bold);
  y = static_cast<int16_t>(y + titleLines * lh2 + 6);

  const int16_t lh1 = lineHeight(style::kSmall.scale);
  const int16_t fh1 = fontH(style::kSmall.scale);
  const int16_t msgMax = std::max<int16_t>(
      0, std::min<int16_t>(3, static_cast<int16_t>((geom::kContentBottom - y) / lh1)));
  const int16_t msgLines = drawTextWrapped(c, geom::kInnerX, y, geom::kInnerW, m.message.c_str(),
                                           style::kSmall.scale, lh1, msgMax, style::kSmall.gray);
  y = static_cast<int16_t>(y + msgLines * lh1 + 8);

  // Meta: one column (two columns of 2x text leave no room for values).
  size_t i = 0;
  for (; i < m.meta.size() && i < 8; i++) {
    if (y + fh1 > geom::kContentBottom) break;
    textFit(c, geom::kInnerX, y, static_cast<int16_t>(kKvLabelW - 8), m.meta[i].first.c_str(),
            style::kCaption);
    textFit(c, static_cast<int16_t>(geom::kInnerX + kKvLabelW), y, kvValueW(),
            m.meta[i].second.c_str(), style::kSmall);
    y = static_cast<int16_t>(y + lh1);
  }
  if (i > 0) y = static_cast<int16_t>(y + 6);

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

void drawStatus(PortraitCanvas& c, const KvModel& m) {
  drawPageTitle(c, Str::PageStatus);
  if (m.pageText[0] != '\0') {
    textRight(c, geom::kInnerRight, centerY(geom::kTitleY, geom::kTitleH, style::kCaption.scale),
              120, m.pageText, style::kCaption);
  }
  const int16_t y = drawKvList(c, m.rows, geom::kBodyTop);
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

std::vector<std::pair<std::string, std::string>> helpRows() {
  static constexpr std::pair<Str, Str> kRows[] = {
      {Str::HelpUpDownShort, Str::HelpScreen},     {Str::HelpUpDownLong, Str::HelpFocus},
      {Str::HelpPower, Str::HelpActivateBack},     {Str::HelpPowerLong, Str::HelpShutDown},
      {Str::HelpFrontHome, Str::HelpHome},         {Str::HelpFrontGestures, Str::HelpSettingsStatus},
      {Str::HelpFrontDouble, Str::HelpStatusPage}, {Str::HelpSwipe, Str::HelpPage},
      {Str::HelpTapAlert, Str::HelpOpenDetail},    {Str::HelpUpDownChord, Str::HelpKeepAwake},
      {Str::HelpDanger, Str::HelpFreshPress},      {Str::HelpCustomBindings, Str::HelpWebBindings},
  };
  std::vector<std::pair<std::string, std::string>> rows;
  for (const auto& [label, value] : kRows) rows.emplace_back(tr(label), tr(value));
  return rows;
}

void drawHelp(PortraitCanvas& c, const KvModel& m) {
  drawPageTitle(c, Str::PageHelp);
  drawKvList(c, m.rows, geom::kBodyTop);
}

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
      const TextStyle vs = valueSt;
      textRight(c, valueRight, centerY(ry, geom::kSettingsRowH, vs.scale), valueMax,
                row.value.c_str(), vs);
    }
    if (row.isAction) {
      c.drawIcon(static_cast<int16_t>(boxX + boxW - geom::kRowPadX - 16),
                 static_cast<int16_t>(ry + (geom::kSettingsRowH - 16) / 2),
                 eego::ICON_CHEVRON_RIGHT_16, 16, 16, 0);
    }
  }

  char page[32];
  snprintf(page, sizeof(page), tr(Str::SettingsPageFmt), static_cast<unsigned>(m.page + 1),
           static_cast<unsigned>(m.pageCount));
  textCentered(c, geom::kContentX, geom::kContentW,
               centerY(geom::kSettingsPagerY, geom::kSettingsPagerH, style::kBody.scale), page,
               style::kBody);
}

//

void drawOnboarding(PortraitCanvas& c, const OnboardingModel& m) {
  drawPageTitle(c, Str::PageOnboarding);
  int16_t y = geom::kBodyTop;

  if (!m.asleep) textFit(c, geom::kInnerX, y, geom::kInnerW, tr(Str::OnbOpen), style::kCaption);
  y = static_cast<int16_t>(y + lineHeight(style::kCaption.scale) + 4);

  const int16_t boxH = 84;
  c.drawRoundRect(geom::kContentX, y, geom::kContentW, boxH, 10, 0);
  c.drawRoundRect(static_cast<int16_t>(geom::kContentX + 1), static_cast<int16_t>(y + 1),
                  static_cast<int16_t>(geom::kContentW - 2), static_cast<int16_t>(boxH - 2), 9, 0);
  const char* address = m.asleep        ? tr(Str::OnbAsleep)
                        : m.url.empty() ? tr(Str::OnbNoAddress)
                                        : m.url.c_str();
  TextStyle addr = m.asleep || m.url.empty() ? style::kBodyMuted : style::kHeadline;
  const int16_t boxInner = static_cast<int16_t>(geom::kContentW - 2 * geom::kRowPadX);
  while (addr.scale > 1 && (styleW(address, addr) > boxInner || fontH(addr.scale) > boxH - 12)) {
    addr.scale = static_cast<int16_t>(addr.scale - 1);
  }
  textCentered(c, geom::kContentX, geom::kContentW, centerY(y, boxH, addr.scale), address, addr);
  y = static_cast<int16_t>(y + boxH + 12);

  const std::string network = m.asleep ? std::string(tr(Str::OnbAsleepNote)) : m.network;
  if (!network.empty()) {
    textFit(c, geom::kInnerX, y, geom::kInnerW, network.c_str(), style::kBodyMuted);
    y = static_cast<int16_t>(y + lineHeight(style::kBodyMuted.scale) + 12);
  }

  hline(c, geom::kInnerX, y, geom::kInnerW, kDividerGray);
  y = static_cast<int16_t>(y + 16);
  const Str lanSteps[3] = {Str::OnbStep1, Str::OnbStep2, Str::OnbStep3};
  const Str apSteps[3] = {Str::OnbApStep1, Str::OnbApStep2, Str::OnbApStep3};
  const Str* steps = m.apMode ? apSteps : lanSteps;
  const int16_t lh = lineHeight(style::kBody.scale);
  const int16_t footLh = lineHeight(style::kCaption.scale);
  const int16_t footLines = static_cast<int16_t>(
      wrapLines(tr(Str::OnbKeys), style::kCaption.scale, geom::kInnerW, 2).size());
  const int16_t footerY =
      static_cast<int16_t>(geom::kContentBottom - std::max<int16_t>(1, footLines) * footLh);
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

  if (!m.asleep) {
    drawTextWrapped(c, geom::kInnerX, footerY, geom::kInnerW, tr(Str::OnbKeys),
                    style::kCaption.scale, footLh, 2, style::kCaption.gray);
  }
}

void drawPoweredOff(PortraitCanvas& c) {
  c.clear();
  c.useUiContentRect();
  const int16_t titleLh = lineHeight(style::kTitle.scale);
  const int16_t y =
      static_cast<int16_t>((geom::kContentTop + geom::kContentBottom - titleLh) / 2);
  textCentered(c, geom::kInnerX, geom::kInnerW, y, tr(Str::PoweredOff), style::kTitle);
  textCentered(c, geom::kInnerX, geom::kInnerW, static_cast<int16_t>(y + titleLh + 8),
               tr(Str::PowerOnHint), style::kCaption);
}

void drawProvision(PortraitCanvas& c, const ProvisionModel& m) {
  if (m.succeeded) {
    const int16_t y = centerY(geom::kBodyTop,
                              static_cast<int16_t>(geom::kContentBottom - geom::kBodyTop),
                              style::kTitle.scale);
    textCentered(c, geom::kInnerX, geom::kInnerW, y, tr(Str::ProvDone), style::kTitle);
    if (m.statusText[0] != '\0') {
      const int16_t lh = lineHeight(style::kBody.scale);
      drawTextWrapped(c, geom::kInnerX,
                      static_cast<int16_t>(y + lineHeight(style::kTitle.scale) + 24),
                      geom::kInnerW, m.statusText, style::kBody.scale, lh, 3, style::kBody.gray);
    }
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
  if (m.statusText[0] != '\0') {
    const int16_t n = drawTextWrapped(c, geom::kInnerX, static_cast<int16_t>(y + 8),
                                      geom::kInnerW, m.statusText, style::kBodyStrong.scale, lh,
                                      3, style::kBodyStrong.gray);
    y = static_cast<int16_t>(y + 8 + std::max<int16_t>(n, 1) * lh + 16);
  }
  TextStyle win = style::kCaption;
  textFit(c, geom::kInnerX, static_cast<int16_t>(y + 8), geom::kInnerW, m.windowText, win);
}

void drawBlePair(PortraitCanvas& c, const BlePairModel& m) {
  drawPageTitle(c, Str::BlePairTitle);
  c.drawLine(geom::kInnerX, static_cast<int16_t>(geom::kBodyTop - 2),
             static_cast<int16_t>(geom::kInnerRight - 1), static_cast<int16_t>(geom::kBodyTop - 2),
             0);
  const int16_t lh = lineHeight(style::kBody.scale);
  int16_t y = static_cast<int16_t>(geom::kBodyTop + 24);
  const int16_t n = drawTextWrapped(c, geom::kInnerX, y, geom::kInnerW, tr(Str::BlePairHint),
                                    style::kBody.scale, lh, 3, style::kBody.gray);
  y = static_cast<int16_t>(y + std::max<int16_t>(n, 1) * lh + 40);

  TextStyle code{8, true, 0};
  while (code.scale > 2 && styleW(m.passkey, code) > geom::kInnerW) {
    code.scale = static_cast<int16_t>(code.scale - 1);
  }
  textCentered(c, geom::kInnerX, geom::kInnerW, y, m.passkey, code);
  y = static_cast<int16_t>(y + fontH(code.scale) + 40);

  if (m.deviceName[0] != '\0') {
    textCentered(c, geom::kInnerX, geom::kInnerW, y, m.deviceName, style::kBodyMuted);
    y = static_cast<int16_t>(y + lh + 8);
  }
  char line[64];
  snprintf(line, sizeof(line), tr(Str::BlePairExpiresFmt), static_cast<unsigned>(m.remainingS));
  textCentered(c, geom::kInnerX, geom::kInnerW, y, line, style::kCaption);
}

void drawNoticeScreen(PortraitCanvas& c, const char* title, const char* line1,
                      const char* line2, const char* line3) {
  // Full-screen notices are read from across the room: headline title, scale-3 body
  // (wrapping to two lines), the wake hint in gray.
  c.clear();
  c.useUiContentRect();
  constexpr int16_t kTitleScale = 4;
  constexpr int16_t kLineScale = 3;
  const int16_t titleLh = lineHeight(kTitleScale);
  const int16_t lineLh = lineHeight(kLineScale);
  const int16_t gap = 24;
  const int16_t blockH = static_cast<int16_t>(titleLh + gap + 4 * lineLh + 12);
  int16_t y = static_cast<int16_t>((geom::kContentTop + geom::kContentBottom - blockH) / 2);
  drawTextFit(c, geom::kInnerX, y, geom::kInnerW, title, kTitleScale, true, 0);
  y = static_cast<int16_t>(y + titleLh + gap);
  const char* lines[] = {line1, line2, line3};
  for (int i = 0; i < 3; ++i) {
    const char* line = lines[i];
    if (line == nullptr || line[0] == '\0') continue;
    const bool hint = i == 2;
    if (hint) y = static_cast<int16_t>(y + 12);
    const int16_t n = drawTextWrapped(c, geom::kInnerX, y, geom::kInnerW, line, kLineScale,
                                      lineLh, i == 0 ? 2 : 1, hint ? 6 : 0, i == 0);
    y = static_cast<int16_t>(y + std::max<int16_t>(n, 1) * lineLh);
  }
  c.resetClip();
}

}  // namespace panel
