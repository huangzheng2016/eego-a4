// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// SHA-1 over arbitrary bytes, returned as 40 lowercase hex chars.
//
// Used only for non-cryptographic alert fingerprints (dedup identity), never
// for security purposes — the security design (M13) does not rely on it.

#include <cstddef>
#include <string>

namespace alert_core {

std::string sha1Hex(const void* data, size_t len);
std::string sha1Hex(const std::string& s);

}  // namespace alert_core
