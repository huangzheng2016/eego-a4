// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

#include "dashboard_core/json.h"
#include "data_pipeline/types.h"

namespace eego::dashboard::pipeline {

bool parseSourceResponse(const SourcePlan& plan, const HttpResponse& response,
                         const VariableValues& variables, int64_t now_ms,
                         SourceResult& out, std::string& error);

// Numeric timestamps may be seconds or milliseconds. ISO-8601 strings with Z or
// an explicit numeric offset are accepted without relying on host timezone state.
bool parseTimestamp(const JsonValue& value, int64_t& timestamp_ms);

}  // namespace eego::dashboard::pipeline
