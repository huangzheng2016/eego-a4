// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// UTC ISO8601 timestamps over pure civil-calendar arithmetic — no libc
// timezone state, identical results on host and device.
//
// Storage format is unix seconds (UTC). Wire/storage text format is
// "YYYY-MM-DDTHH:MM:SSZ"; the parser also accepts fractional seconds and
// explicit "+HH:MM"/"-HH:MM" offsets, which are normalized to UTC.

#include <cstdint>
#include <string>

namespace alert_core {

// "YYYY-MM-DDTHH:MM:SSZ" from unix seconds.
std::string formatUtcIso8601(uint64_t unix_seconds);

// Parses "YYYY-MM-DDTHH:MM:SS[.fff][Z|+HH:MM|-HH:MM]". Returns false on any
// syntax or range violation (including pre-1970 results). Fractional seconds
// are truncated; leap seconds (:60) are rejected.
bool parseUtcIso8601(const std::string& s, uint64_t& unix_seconds_out);

}  // namespace alert_core
