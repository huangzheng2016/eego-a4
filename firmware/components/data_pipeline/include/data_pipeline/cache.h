// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "data_pipeline/history.h"

namespace eego::dashboard::pipeline {

struct CacheLimits {
  size_t max_history_points = 8192;
  size_t downsample_points = 2048;
  size_t max_history_bytes = 16 * 1024 * 1024;  // bounded full 1s fixed_5h coverage
  size_t max_snapshot_bytes = 512 * 1024;
  size_t max_image_bytes = kMaxImageBytes;
  int32_t timezone_offset_minutes = 0;
};

// stdio maps to VFS on-device and ordinary files in host fixtures. Every
// replacement uses tmp+rename; filenames are derived only from validated IDs.
class SdCache {
 public:
  explicit SdCache(std::string directory, CacheLimits limits = {});

  bool appendHistory(const std::string& source_id, const std::vector<HistoryEntry>& entries,
                     std::string& error);
  bool loadHistory(const std::string& source_id, std::vector<HistoryEntry>& out,
                   std::string& error) const;

  bool writeSnapshot(const SourceResult& result, std::string& error);
  bool readSnapshot(const std::string& source_id, SourceResult& out, std::string& error) const;

  const std::string& directory() const { return directory_; }
  const CacheLimits& limits() const { return limits_; }
  void setTimezoneOffset(int32_t minutes) { limits_.timezone_offset_minutes = minutes; }

 private:
  std::string historyPath(const std::string& source_id) const;
  std::string snapshotPath(const std::string& source_id) const;
  std::string imagePath(const std::string& source_id) const;
  std::string imageGenerationPath(const std::string& source_id,
                                  const std::string& generation) const;
  void cleanupImages(const std::string& source_id, const std::string& keep_generation) const;
  bool loadHistoryLimited(const std::string& source_id, std::vector<HistoryEntry>& out,
                          std::string& error, size_t max_bytes) const;

  std::string directory_;
  CacheLimits limits_;
};

}  // namespace eego::dashboard::pipeline
