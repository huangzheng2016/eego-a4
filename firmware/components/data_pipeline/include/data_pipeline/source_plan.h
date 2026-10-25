// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>

#include "dashboard_core/json.h"
#include "data_pipeline/types.h"

namespace eego::dashboard::pipeline {

bool validateSourcePlan(const SourcePlan& plan, std::string& error);
bool sourcePlanFromJson(const JsonValue& value, SourcePlan& out, std::string& error);

// Interpolation is single-pass and inherited from dashboard_core. Authentication
// is resolved only while building the in-memory request and is never persisted.
bool buildSourceRequest(const SourcePlan& plan, const VariableValues& variables,
                        const TokenResolver& token_resolver, net::NetRequest& out,
                        std::string& error);
std::string sourceRequestKey(const net::NetRequest& request);

SourcePlan makeWeatherPlan(const std::string& id, const std::string& url);

}  // namespace eego::dashboard::pipeline
