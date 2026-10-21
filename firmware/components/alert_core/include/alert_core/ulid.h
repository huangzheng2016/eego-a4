// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Crockford-base32 ULIDs: 48-bit unix-ms timestamp + 80-bit randomness,
// 26 chars, e.g. "01JBX8K2M0Q7T3N6P1V9W4Y5Z".
//
// Clock and entropy are caller-supplied so the core stays free of platform
// dependencies: the ingest owner passes its RTC/NTP time and an RNG hook.

#include <cstddef>
#include <cstdint>
#include <string>

namespace alert_core {

constexpr size_t kUlidStrLen = 26;

// Encodes one ULID from a unix-ms timestamp and 16 bytes of entropy.
std::string ulidEncode(uint64_t unix_ms, const uint8_t entropy[16]);

// Monotonic generator: within the same millisecond the randomness component is
// incremented instead of re-drawn, and timestamps never move backwards, so
// ids generated in one clock tick still sort strictly by creation order.
class UlidMonotonic {
 public:
  std::string next(uint64_t unix_ms, const uint8_t entropy[16]);

 private:
  uint64_t last_ms_ = 0;
  uint8_t last_rand_[16] = {0};
  bool have_prev_ = false;
};

}  // namespace alert_core
