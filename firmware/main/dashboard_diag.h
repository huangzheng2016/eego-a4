// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace panel {

inline bool dashboardSensitiveVariableName(std::string name) {
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  auto ends = [&](const char* suffix) {
    const std::string tail(suffix);
    return name.size() >= tail.size() && name.compare(name.size() - tail.size(), tail.size(), tail) == 0;
  };
  std::string compact;
  compact.reserve(name.size());
  for (const unsigned char c : name) {
    if (std::isalnum(c)) compact.push_back(static_cast<char>(c));
  }
  auto endsCompact = [&](const char* suffix) {
    const std::string tail(suffix);
    return compact.size() >= tail.size() &&
           compact.compare(compact.size() - tail.size(), tail.size(), tail) == 0;
  };
  if (ends("_ref") || compact == "dedupkey") return false;
  for (const char* marker : {"password", "passwd", "passphrase", "secret", "token", "auth",
                             "credential", "psk"}) {
    if (compact.find(marker) != std::string::npos) return true;
  }
  if (compact == "key" || compact == "pass" || compact == "pwd") return true;
  if (endsCompact("key")) return true;
  return endsCompact("pass") || endsCompact("passwd") || endsCompact("pwd");
}

}  // namespace panel
