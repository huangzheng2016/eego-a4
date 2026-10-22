// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

#include "esp_err.h"

namespace eego::admin::internal {

bool sourceCredentialInUse(const std::string& key);
esp_err_t deleteSourceCredentialIfUnreferenced(const std::string& key, bool& in_use);

}  // namespace eego::admin::internal
