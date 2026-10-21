// SPDX-License-Identifier: GPL-3.0-or-later
// 源离线缓存实现（tmp+rename 原子写，stdio/VFS）。
#include "net_stack/source_cache.h"

#include <sys/stat.h>
#include <cctype>
#include <cstdio>

#include "esp_log.h"

namespace eego::net {

static const char* TAG = "net.cache";

SourceCache::SourceCache(std::string dir) : dir_(std::move(dir)) {
  if (!dir_.empty() && dir_.back() == '/') dir_.pop_back();
}

bool SourceCache::validSourceId(const std::string& id) {
  if (id.empty() || id.size() > 64) return false;
  for (char c : id) {
    if (!std::isalnum((unsigned char)c) && c != '-' && c != '_') return false;
  }
  return true;
}

std::string SourceCache::pathFor(const std::string& source_id) const {
  return dir_ + "/" + source_id + ".json";
}

bool SourceCache::write(const std::string& source_id, const char* data, size_t len,
                        std::string& err) {
  if (!validSourceId(source_id)) {
    err = "invalid source id";
    return false;
  }
  std::string tmp = pathFor(source_id) + ".tmp";
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f) {
    err = "open tmp failed";
    return false;
  }
  size_t n = fwrite(data, 1, len, f);
  bool ok = (n == len) && (fflush(f) == 0);
  fclose(f);
  if (!ok) {
    err = "write failed";
    remove(tmp.c_str());
    return false;
  }
  if (rename(tmp.c_str(), pathFor(source_id).c_str()) != 0) {
    err = "rename failed";
    remove(tmp.c_str());
    return false;
  }
  return true;
}

bool SourceCache::read(const std::string& source_id, std::string& body_out,
                       uint64_t& fetched_at_out) const {
  if (!validSourceId(source_id)) return false;
  struct stat st;
  if (stat(pathFor(source_id).c_str(), &st) != 0) return false;
  FILE* f = fopen(pathFor(source_id).c_str(), "rb");
  if (!f) return false;
  body_out.resize(st.st_size > 0 ? (size_t)st.st_size : 0);
  size_t n = body_out.empty() ? 0 : fread(body_out.data(), 1, body_out.size(), f);
  fclose(f);
  body_out.resize(n);
  fetched_at_out = st.st_mtime > 0 ? (uint64_t)st.st_mtime : 0;
  return true;
}

bool SourceCache::remove(const std::string& source_id) {
  if (!validSourceId(source_id)) return false;
  return ::remove(pathFor(source_id).c_str()) == 0;
}

}  // namespace eego::net
