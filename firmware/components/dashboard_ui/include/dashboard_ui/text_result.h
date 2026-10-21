// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace eego::dashboard::ui {

inline std::string literalTextForTemplate(std::string_view input) {
  std::string out;
  out.reserve(input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '$' && i + 1 < input.size() && input[i + 1] == '{') out += "$$";
    else
      out.push_back(input[i]);
  }
  return out;
}

}  // namespace eego::dashboard::ui
