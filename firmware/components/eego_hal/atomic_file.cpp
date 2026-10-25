// SPDX-License-Identifier: GPL-3.0-or-later
#include "atomic_file.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>

namespace eego::fsutil {
namespace {

class PosixFileOps final : public FileOps {
 public:
  bool exists(const std::string& path) override {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
  }
  bool rename(const std::string& from, const std::string& to) override {
    return std::rename(from.c_str(), to.c_str()) == 0;
  }
  bool remove(const std::string& path) override { return std::remove(path.c_str()) == 0; }
  bool writeSync(const std::string& path, const void* data, size_t size) override {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) return false;
    bool ok = size == 0 || std::fwrite(data, 1, size, file) == size;
    if (ok) ok = std::fflush(file) == 0;
    if (ok) ok = fsync(fileno(file)) == 0;
    if (std::fclose(file) != 0) ok = false;
    return ok;
  }
};

}  // namespace

FileOps& posixFileOps() {
  static PosixFileOps ops;
  return ops;
}

bool replaceFile(const std::string& tmp, const std::string& target, FileOps& ops) {
  if (!ops.exists(target)) return ops.rename(tmp, target);
  const std::string backup = target + kBackupSuffix;
  if (ops.exists(backup)) ops.remove(backup);  // stale from an interrupted replace
  if (!ops.rename(target, backup)) return false;
  if (!ops.rename(tmp, target)) {
    ops.rename(backup, target);  // roll back; on failure recoverFile() restores it
    return false;
  }
  ops.remove(backup);
  return true;
}

bool atomicWriteFile(const std::string& target, const void* data, size_t size, FileOps& ops) {
  const std::string tmp = target + kTempSuffix;
  if (!ops.writeSync(tmp, data, size) || !replaceFile(tmp, target, ops)) {
    ops.remove(tmp);
    return false;
  }
  return true;
}

bool recoverFile(const std::string& target, FileOps& ops) {
  if (ops.exists(target)) return false;
  const std::string backup = target + kBackupSuffix;
  return ops.exists(backup) && ops.rename(backup, target);
}

}  // namespace eego::fsutil
