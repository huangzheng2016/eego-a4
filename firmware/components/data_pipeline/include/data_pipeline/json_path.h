// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "dashboard_core/json.h"
#include "data_pipeline/types.h"

namespace eego::dashboard::pipeline {

// Bounded JSONPath subset: $, dot/bracket children, array indices, and [*].
// Filters, recursive descent, slices, unions, and script expressions are rejected.
bool validateJsonPath(std::string_view path, std::string& error);
bool selectJsonPath(const JsonValue& root, std::string_view path,
                    std::vector<const JsonValue*>& out, std::string& error,
                    size_t max_results = kMaxMappingRecords);
const JsonValue* jsonPathFirst(const JsonValue& root, std::string_view path);

}  // namespace eego::dashboard::pipeline
