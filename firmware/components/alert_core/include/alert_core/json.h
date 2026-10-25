// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// alert_core uses the shared JSON DOM (components/json_dom): strict RFC 8259
// (no comments, trailing commas, duplicate keys or trailing content; nesting
// depth capped). Parse errors carry a byte offset and message so the ingest
// layer can turn them into field-level 400 responses.
#include "json_dom/json.h"

namespace alert_core {

using eego::json::JsonParseError;
using eego::json::JsonValue;
using eego::json::jsonCodePointCount;
using eego::json::jsonEscape;
using eego::json::jsonIsValidUtf8;
using eego::json::jsonParse;

}  // namespace alert_core
