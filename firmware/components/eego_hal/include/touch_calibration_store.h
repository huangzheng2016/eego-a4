// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <esp_err.h>

#include "touch_calibration.h"

namespace eego::touchcal {
constexpr const char* kNvsNamespace = "touchcal";
constexpr const char* kNvsKey = "affine";

esp_err_t loadPersisted(Affine& out);
esp_err_t savePersisted(const Affine& affine);
esp_err_t erasePersisted();

}  // namespace eego::touchcal
