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
  if (ends("_ref")) return false;
  for (const char* marker : {"password", "passwd", "passphrase", "secret", "token", "auth",
                             "credential", "psk"}) {
    if (name.find(marker) != std::string::npos) return true;
  }
  if (name == "key" || name == "apikey" || name == "api-key" || name == "ingestkey" ||
      name == "accesskey" || name == "wifikey" || name == "pwd") {
    return true;
  }
  if (ends("_key") && name != "dedup_key") return true;
  return ends("_pwd") || ends("_pass");
}

}  // namespace panel
