// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "dashboard_core/config.h"

namespace eego::dashboard {

bool migrateV1ToV2(const JsonValue& input, JsonValue& output,
                   std::vector<ValidationError>& errors);

}  // namespace eego::dashboard
