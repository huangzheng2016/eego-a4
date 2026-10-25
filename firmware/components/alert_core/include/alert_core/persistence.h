// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// alerts.db persistence: JSONL append + compaction (M11 §4: "SD alerts.db,
// JSONL 追加 + 定期压缩").
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
// Compaction rewrites the live set: stream one line per record into tmp →
// stream it back verifying byte count, line count and an FNV-1a hash of what
// was written → rename over the original; any failure removes the tmp file and
// leaves the original intact (rollback). Neither load nor compaction holds the
// whole file in RAM (internal-heap only on device): both stream in small
// chunks, so peak memory is one record line plus the live record set.

#include <cstddef>
#include <cstdint>
#include <functional>
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
  // Same-directory rename that replaces an existing `to`. POSIX rename does this
  // atomically; on FatFS (rename never overwrites) SdAlertFileIo goes through
  // eego_hal atomic_file replaceFile (to -> to.bak, from -> to, drop .bak).
  virtual bool rename(const std::string& from, const std::string& to) = 0;
  virtual bool fileSize(const std::string& path, size_t& out) const = 0;

  // Streaming access (bounded RAM). A ChunkSink returns false to abort.
  using ChunkSink = std::function<bool(const char* data, size_t n)>;
  // Creates or truncates `path`, then calls `producer` with a sink that writes
  // to the open file; flushes/syncs and closes afterwards. False when opening,
  // any write, the producer, or close fails.
  virtual bool writeStream(const std::string& path,
                           const std::function<bool(const ChunkSink& sink)>& producer) = 0;
  // Reads `path` front to back in small chunks into `sink`. False when missing,
  // unreadable, or the sink aborts.
  virtual bool readStream(const std::string& path, const ChunkSink& sink) = 0;
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
  // Visitor form: records are pulled one at a time through `at(i)` for
  // i in [0, count) (no copy of the live set). `at` must stay valid and return
  // non-null for the whole call — the single store owner calls this inline.
  using RecordAt = std::function<const AlertRecord*(size_t index)>;
  bool compact(size_t count, const RecordAt& at, std::string& err);

  // Failed write/rename operations since construction — the ingest owner
  // polls this (or IngestOutcome::persist_failed) to react to SD trouble
  // instead of silently diverging from memory.
  size_t ioFailures() const { return io_failures_; }

  // Current on-disk size of the db file; false when missing/unreadable.
  bool fileSize(size_t& out) const { return io_.fileSize(path_, out); }

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

}  // namespace alert_core
