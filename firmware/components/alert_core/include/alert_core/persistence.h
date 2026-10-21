#pragma once

// alerts.db persistence: JSONL append + compaction (M11 §4: "SD alerts.db,
// JSONL 追加 + 定期压缩") and the cache/<source>.json offline-cache helpers.
//
// File access goes through IAlertFileIo so alert_core stays free of SD/FatFS
// or any other platform dependency. The firmware wires an SD-backed
// implementation (M14/M16); host tools use StdioFileIo (file_io_stdio.h).
//
// alerts.db format: one canonical AlertRecord JSON per line, append-only
// upsert — loading applies lines in order and the last line for an id wins,
// so a torn tail loses at most the newest mutations and never corrupts older
// records. Ring evictions append `{"tombstone":"<id>"}` lines; tombstoned ids
// never re-enter the live set, so the retention ring survives reboot.
// Compaction rewrites the live set: write tmp → read back → verify
// (line count and id sequence) → rename over the original; any failure
// removes the tmp file and leaves the original intact (rollback).

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "alert_core/types.h"

namespace alert_core {

class IAlertFileIo {
 public:
  virtual ~IAlertFileIo() = default;

  // Creates or truncates `path` and writes `data` in full.
  virtual bool writeAll(const std::string& path, const std::string& data) = 0;
  // Appends `data`, creating `path` if needed.
  virtual bool append(const std::string& path, const std::string& data) = 0;
  // Reads the whole file. Returns false when missing or unreadable.
  virtual bool readAll(const std::string& path, std::string& out) = 0;
  virtual bool exists(const std::string& path) const = 0;
  virtual bool remove(const std::string& path) = 0;
  // Same-directory rename (atomic on POSIX and FatFS).
  virtual bool rename(const std::string& from, const std::string& to) = 0;
  virtual bool fileSize(const std::string& path, size_t& out) const = 0;
};

class AlertDb {
 public:
  AlertDb(IAlertFileIo& io, std::string path);

  const std::string& path() const { return path_; }

  // Appends one full-record line (upsert-on-load semantics).
  bool appendRecord(const AlertRecord& r);

  // Appends a tombstone line for an evicted id so ring eviction survives
  // reboot: on load, tombstoned ids never re-enter the live set, whether the
  // tombstone lands before or after the record's own lines.
  bool appendTombstone(const std::string& id);

  // Loads the live record set: lines applied in order, last per id wins,
  // tombstones remove ids; malformed lines are skipped and counted in
  // `malformed_out`.
  size_t load(std::vector<AlertRecord>& out, size_t& malformed_out);

  // Rewrites the db with exactly `records` via tmp + read-back verify +
  // rename. On failure `err` explains and the original file is untouched.
  bool compact(const std::vector<AlertRecord>& records, std::string& err);

  // Failed write/rename operations since construction — the ingest owner
  // polls this (or IngestOutcome::persist_failed) to react to SD trouble
  // instead of silently diverging from memory.
  size_t ioFailures() const { return io_failures_; }

  // Appended-but-superseded lines since construction/load; the ingest owner
  // uses this to decide when to compact (e.g. dead ratio > 50%).
  size_t deadLines() const { return dead_lines_; }
  size_t totalLines() const { return total_lines_; }
  void resetLineCounters() {
    dead_lines_ = 0;
    total_lines_ = 0;
  }

 private:
  IAlertFileIo& io_;
  std::string path_;
  size_t dead_lines_ = 0;
  size_t total_lines_ = 0;
  size_t io_failures_ = 0;
  std::unordered_set<std::string> live_ids_;  // ids with a live (latest) line
};

// Offline cache for http_poll sources (M11 §2): cache/<source>.json holding
// the last good response body. source_id is restricted to [A-Za-z0-9_-] so it
// can never escape the cache directory.
bool writeSourceCache(IAlertFileIo& io, const std::string& dir, const std::string& source_id,
                      const std::string& body, std::string& err);
bool readSourceCache(IAlertFileIo& io, const std::string& dir, const std::string& source_id,
                     std::string& body_out);
bool removeSourceCache(IAlertFileIo& io, const std::string& dir, const std::string& source_id);

}  // namespace alert_core
