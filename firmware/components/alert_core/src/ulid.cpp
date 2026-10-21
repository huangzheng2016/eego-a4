#include "alert_core/ulid.h"

#include <cstring>

namespace alert_core {
namespace {

constexpr char kEncoding[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

void shiftRight5(uint8_t r[16]) {
  uint8_t prev_low = 0;
  for (int b = 0; b < 16; ++b) {
    uint8_t cur_low = static_cast<uint8_t>(r[b] & 0x1F);
    r[b] = static_cast<uint8_t>((r[b] >> 5) | (prev_low << 3));
    prev_low = cur_low;
  }
}

void incrementBigEndian(uint8_t r[16]) {
  for (int b = 15; b >= 0; --b) {
    if (++r[b] != 0) break;
  }
}

std::string encodeFrom(uint64_t unix_ms, const uint8_t rand_bytes[16]) {
  std::string out(kUlidStrLen, '0');
  for (int i = 9; i >= 0; --i) {
    out[static_cast<size_t>(i)] = kEncoding[unix_ms & 0x1F];
    unix_ms >>= 5;
  }
  uint8_t r[16];
  std::memcpy(r, rand_bytes, 16);
  for (int i = 15; i >= 0; --i) {
    out[10 + static_cast<size_t>(i)] = kEncoding[r[15] & 0x1F];
    shiftRight5(r);
  }
  return out;
}

}  // namespace

std::string ulidEncode(uint64_t unix_ms, const uint8_t entropy[16]) {
  return encodeFrom(unix_ms, entropy);
}

std::string UlidMonotonic::next(uint64_t unix_ms, const uint8_t entropy[16]) {
  if (have_prev_ && unix_ms <= last_ms_) {
    // Same (or backwards) clock tick: increment the randomness component so
    // ids stay unique and strictly increasing within the tick.
    incrementBigEndian(last_rand_);
    return encodeFrom(last_ms_, last_rand_);
  }
  last_ms_ = unix_ms;
  std::memcpy(last_rand_, entropy, 16);
  have_prev_ = true;
  return encodeFrom(unix_ms, last_rand_);
}

}  // namespace alert_core
