// SPDX-License-Identifier: GPL-3.0-or-later
// FatFS-safe "write temp, then replace" for files on the SD card.
//
// FatFS f_rename (and therefore ESP-IDF's VFS rename on /sdcard) fails with
// FR_EXIST when the destination exists, so the POSIX "rename tmp over target"
// idiom does not work there. Every replace goes through replaceFile():
//
//   1. the caller (or atomicWriteFile) writes `<target>.tmp`, fflush + fsync + fclose
//   2. target exists → remove stale `<target>.bak`, rename target → `<target>.bak`
//   3. rename tmp → target            (failure: rename .bak back → target)
//   4. remove `<target>.bak`
//
// A power cut between 2 and 3 leaves only `<target>.bak`; recoverFile() (call it
// on boot / before reading) renames it back. A cut between 3 and 4 leaves the
// new target plus a stale .bak, which the next replace removes.
//
// Pure POSIX stdio (no ESP-IDF dependency); the FileOps interface lets host
// tests simulate FatFS's no-overwrite rename.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace eego::fsutil {

inline constexpr const char* kTempSuffix = ".tmp";
inline constexpr const char* kBackupSuffix = ".bak";

class FileOps {
 public:
  virtual ~FileOps() = default;
  virtual bool exists(const std::string& path) = 0;
  // Same-directory rename. May fail when `to` exists (FatFS).
  virtual bool rename(const std::string& from, const std::string& to) = 0;
  virtual bool remove(const std::string& path) = 0;
  // Create/truncate `path` with `data`, then fflush + fsync + fclose.
  virtual bool writeSync(const std::string& path, const void* data, size_t size) = 0;
};

// stdio/VFS implementation (fopen/fwrite/fsync, ::rename, ::remove, stat).
FileOps& posixFileOps();

// Moves the fully written and synced `tmp` onto `target` (steps 2-4 above).
// On failure `target` keeps its previous content (or is restorable from .bak)
// and `tmp` is left for the caller to remove.
bool replaceFile(const std::string& tmp, const std::string& target, FileOps& ops = posixFileOps());

// Writes `<target>.tmp` (synced) and replaceFile()s it onto target; removes the
// temporary file on failure.
bool atomicWriteFile(const std::string& target, const void* data, size_t size,
                     FileOps& ops = posixFileOps());
inline bool atomicWriteFile(const std::string& target, std::string_view data,
                            FileOps& ops = posixFileOps()) {
  return atomicWriteFile(target, data.data(), data.size(), ops);
}

// Interrupted-replace recovery: when `target` is missing but `<target>.bak`
// exists, renames the backup back. Returns true when a backup was restored.
bool recoverFile(const std::string& target, FileOps& ops = posixFileOps());

}  // namespace eego::fsutil
