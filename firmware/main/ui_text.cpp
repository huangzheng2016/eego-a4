// SPDX-License-Identifier: GPL-3.0-or-later
// 中英文案表实现（见 ui_text.h）。
#include "ui_text.h"

#include <cctype>
#include <cstring>
#include <strings.h>  // strcasecmp

namespace panel {

namespace {

struct Entry {
  const char* zh;
  const char* en;
};

constexpr Entry kTable[] = {
#define X(k, zh, en) {zh, en},
    PANEL_STR_LIST(X)
#undef X
};

Lang g_lang = Lang::Zh;

}  // namespace

Lang currentLang() { return g_lang; }

void setLang(Lang lang) { g_lang = (lang == Lang::En) ? Lang::En : Lang::Zh; }

const char* tr(Str s) {
  const size_t i = static_cast<size_t>(s);
  if (i >= sizeof(kTable) / sizeof(kTable[0])) return "";
  return g_lang == Lang::Zh ? kTable[i].zh : kTable[i].en;
}

bool langFromString(const char* s, Lang& out) {
  if (s == nullptr) return false;
  if (strcasecmp(s, "zh") == 0 || strcasecmp(s, "cn") == 0) {
    out = Lang::Zh;
    return true;
  }
  if (strcasecmp(s, "en") == 0) {
    out = Lang::En;
    return true;
  }
  return false;
}

const char* langCode(Lang lang) { return lang == Lang::En ? "en" : "zh"; }

}  // namespace panel
