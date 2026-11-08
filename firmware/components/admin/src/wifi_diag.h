// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

//

#include <string>

namespace eego::admin::wifi_diag {
extern const char kHtml[];  // GET /wifi-diag
extern const char kJs[];    // GET /wifi-diag.js
extern const char kCss[];   // GET /wifi-diag.css

std::string diagJson();

bool retryNow();

bool scanNow();

}  // namespace eego::admin::wifi_diag
