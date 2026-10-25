// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>

// Secret masking for dashboard source previews / probes. Pure string logic (no
// ESP-IDF dependencies) so it is covered by the host tests.
namespace eego::admin::internal {

inline constexpr char kProbeMask[] = "********";

// True when a key name looks like it carries a credential (token, secret,
// pass, key, auth, cookie; case-insensitive substring match).
bool previewSensitive(const std::string& key);

// Plain-text fallback (non-JSON, or JSON too large to parse): wherever a key
// containing a sensitive word is followed by ':' or '=', its value is replaced
// by kProbeMask. Forms:
//   "key": "v" / "key": 12 / "key": {...}  JSON-ish (quoted key)
//   Key: value                              header/YAML style, masked to end of line
//   key=value                               query/form style, stops at , & ; or space
// `masked` is incremented once per masked value.
std::string probeMaskText(const std::string& input, size_t& masked);

}  // namespace eego::admin::internal
