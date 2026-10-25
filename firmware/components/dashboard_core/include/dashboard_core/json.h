// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// dashboard_core uses the shared JSON DOM (components/json_dom).
#include "json_dom/json.h"

namespace eego::dashboard {

using eego::json::JsonParseError;
using eego::json::JsonValue;
using eego::json::jsonCodePointCount;
using eego::json::jsonEscape;
using eego::json::jsonIsValidUtf8;
using eego::json::jsonParse;

}  // namespace eego::dashboard
