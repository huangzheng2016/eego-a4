// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace eego::admin::internal {

// Matches the handler's segment semantics: repeated and trailing slashes are
// collapsed before rate-limit classification and dispatch.
inline std::string canonicalDashboardApiPath(const std::string& path) {
  std::string canonical;
  canonical.reserve(path.size());
  bool slash = false;
  for (const char c : path) {
    if (c == '/') {
      if (!slash && !canonical.empty()) canonical.push_back('/');
      slash = true;
    } else {
      canonical.push_back(c);
      slash = false;
    }
  }
  if (canonical.size() > 1 && canonical.back() == '/') canonical.pop_back();
  return canonical;
}

}  // namespace eego::admin::internal
