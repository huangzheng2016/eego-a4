// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>

#include "esp_err.h"

namespace eego::admin::devlog {
void install();

std::string tail(size_t max_bytes);

// {"reset_reason":"panic","crashed":true,"task":"...","pc":"0x...","backtrace":["0x..."],
std::string crashJson();

esp_err_t clearCrash();

void logBootReport();

}  // namespace eego::admin::devlog
