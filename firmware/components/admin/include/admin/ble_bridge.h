// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "esp_err.h"

namespace eego::admin {
esp_err_t startBleBridge();
void stopBleBridge();

}  // namespace eego::admin
