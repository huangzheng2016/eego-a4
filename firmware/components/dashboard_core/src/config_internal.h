// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "dashboard_core/config.h"

namespace eego::dashboard {

// Programmatic models are serialized into the canonical DOM and passed through
// the same field reader as imported documents. Import itself only needs the
// semantic pass, avoiding recursive validation.
bool parseCanonicalFields(const JsonValue& root, Dashboard& out, const ParseOptions& options,
                          std::vector<ValidationError>& errors);
bool validateDashboardSemantics(const Dashboard& dashboard, const Limits& limits,
                                std::vector<ValidationError>& errors);

}  // namespace eego::dashboard
