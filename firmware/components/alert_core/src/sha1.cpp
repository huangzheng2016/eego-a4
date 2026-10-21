#include "alert_core/sha1.h"

#include <cstdint>
#include <cstring>

namespace alert_core {
namespace {

constexpr uint32_t kBlockBytes = 64;

uint32_t rotl(uint32_t v, int bits) { return (v << bits) | (v >> (32 - bits)); }

void processBlock(uint32_t h[5], const uint8_t block[kBlockBytes]) {
  uint32_t w[80];
  for (int i = 0; i < 16; ++i) {
    w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
           (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
           static_cast<uint32_t>(block[i * 4 + 3]);
  }
  for (int i = 16; i < 80; ++i) {
    w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
  for (int i = 0; i < 80; ++i) {
    uint32_t f, k;
    if (i < 20) {
      f = (b & c) | ((~b) & d);
      k = 0x5A827999u;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1u;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDCu;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6u;
    }
    uint32_t tmp = rotl(a, 5) + f + e + k + w[i];
    e = d;
    d = c;
    c = rotl(b, 30);
    b = a;
    a = tmp;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
}

}  // namespace

std::string sha1Hex(const void* data, size_t len) {
  uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
  const auto* bytes = static_cast<const uint8_t*>(data);

  size_t fullBlocks = len / kBlockBytes;
  for (size_t i = 0; i < fullBlocks; ++i) {
    processBlock(h, bytes + i * kBlockBytes);
  }

  // Final block(s): data remainder + 0x80 pad + zeros + 64-bit bit length.
  uint8_t tail[2 * kBlockBytes] = {0};
  size_t rem = len % kBlockBytes;
  if (rem) std::memcpy(tail, bytes + fullBlocks * kBlockBytes, rem);
  tail[rem] = 0x80;
  size_t tailLen = (rem + 1 + 8 <= kBlockBytes) ? kBlockBytes : 2 * kBlockBytes;
  uint64_t bitLen = static_cast<uint64_t>(len) * 8;
  for (int i = 0; i < 8; ++i) {
    tail[tailLen - 1 - i] = static_cast<uint8_t>((bitLen >> (i * 8)) & 0xFF);
  }
  processBlock(h, tail);
  if (tailLen == 2 * kBlockBytes) processBlock(h, tail + kBlockBytes);

  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(40);
  for (int i = 0; i < 5; ++i) {
    for (int byte = 3; byte >= 0; --byte) {
      uint8_t b = static_cast<uint8_t>((h[i] >> (byte * 8)) & 0xFF);
      out.push_back(kHex[b >> 4]);
      out.push_back(kHex[b & 0xF]);
    }
  }
  return out;
}

std::string sha1Hex(const std::string& s) { return sha1Hex(s.data(), s.size()); }

}  // namespace alert_core
