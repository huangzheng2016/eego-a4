// SPDX-License-Identifier: GPL-3.0-or-later
#include "store_digest.h"

namespace eego::net::internal {
namespace {

// FNV-1a 64 + splitmix64 终结混合：逐记录散列后求和（可交换累积器，entries_
// 顺序变化——如 evict 交换删除——不影响结果；记录增删改变总和）。
constexpr uint64_t kFnvOffset = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

inline uint64_t mix64(uint64_t z) {
  z += 0x9e3779b97f4a7c15ull;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

inline void hashBytes(uint64_t& h, const std::string& s) {
  for (unsigned char c : s) {
    h ^= c;
    h *= kFnvPrime;
  }
}

uint64_t recordDigest(const alert_core::AlertRecord& r) {
  uint64_t h = kFnvOffset;
  hashBytes(h, r.id);
  h ^= static_cast<uint64_t>(r.state);
  h *= kFnvPrime;
  h ^= static_cast<uint64_t>(r.severity);
  h *= kFnvPrime;
  h ^= r.muted ? 0x9dull : 0x1dull;
  h *= kFnvPrime;
  h ^= r.occurrences;
  h *= kFnvPrime;
  h ^= r.last_seen;
  h *= kFnvPrime;
  h ^= r.expires_at;
  h *= kFnvPrime;
  h ^= r.ack_at;
  h *= kFnvPrime;
  h ^= r.cleared_at;
  h *= kFnvPrime;
  return mix64(h);
}

}  // namespace

uint64_t alertStoreDigest(const alert_core::AlertStore& store) {
  // list 空过滤 + include_muted + since=0 + limit=0（0 = 不限）= 全量指针视图，
  // 零拷贝遍历（allRecords 会整表拷贝，摘要路径不需要）。
  const auto all = store.list("", "", "", "", true, 0, 0);
  uint64_t sum = 0;
  for (const alert_core::AlertRecord* r : all) {
    if (r) sum += recordDigest(*r);
  }
  return mix64(sum ^ (static_cast<uint64_t>(all.size()) << 1));
}

}  // namespace eego::net::internal
