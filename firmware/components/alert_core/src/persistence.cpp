// SPDX-License-Identifier: GPL-3.0-or-later
#include "alert_core/persistence.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

#include "alert_core/json.h"

namespace alert_core {

AlertDb::AlertDb(IAlertFileIo& io, std::string path) : io_(io), path_(std::move(path)) {}

bool AlertDb::appendRecord(const AlertRecord& r) {
  const std::string line = recordToJsonText(r);
  if (!io_.append(path_, line + "\n")) {
    ++io_failures_;
    return false;
  }
  ++total_lines_;
  if (!live_ids_.insert(r.id).second) ++dead_lines_;
  return true;
}

bool AlertDb::appendTombstone(const std::string& id) {
  JsonValue::Object obj;
  obj.emplace_back("tombstone", JsonValue::makeString(id));
  const std::string line = JsonValue::makeObject(std::move(obj)).toString();
  if (!io_.append(path_, line + "\n")) {
    ++io_failures_;
    return false;
  }
  ++total_lines_;
  ++dead_lines_;  // tombstones are dead on arrival by nature
  live_ids_.erase(id);
  return true;
}

namespace {

// FNV-1a 64 over the byte stream plus byte/line counts: written vs read back.
struct StreamDigest {
  uint64_t hash = 1469598103934665603ULL;
  size_t bytes = 0;
  size_t lines = 0;
  void add(const char* data, size_t n) {
    for (size_t i = 0; i < n; ++i) {
      hash ^= static_cast<unsigned char>(data[i]);
      hash *= 1099511628211ULL;
      if (data[i] == '\n') ++lines;
    }
    bytes += n;
  }
  bool operator==(const StreamDigest& o) const {
    return hash == o.hash && bytes == o.bytes && lines == o.lines;
  }
};

// A single alerts.db line is one record (a few KB at most); anything longer is
// garbage and is skipped as malformed instead of growing the line buffer.
constexpr size_t kMaxLineBytes = 64 * 1024;

}  // namespace

size_t AlertDb::load(std::vector<AlertRecord>& out, size_t& malformed_out) {
  out.clear();
  malformed_out = 0;
  total_lines_ = 0;
  dead_lines_ = 0;

  std::unordered_map<std::string, size_t> index_by_id;
  std::unordered_set<std::string> tombstoned;

  auto apply_line = [&](const std::string& line) {
    if (line.empty()) return;
    ++total_lines_;
    JsonValue v;
    JsonParseError perr;
    if (!jsonParse(line, v, perr)) {
      ++malformed_out;
      return;
    }
    if (const JsonValue* t = v.find("tombstone");
        t != nullptr && t->type() == JsonValue::Type::String && !t->stringValue().empty()) {
      tombstoned.insert(t->stringValue());
      return;
    }
    AlertRecord r;
    if (!recordFromJson(v, r)) {
      ++malformed_out;
      return;
    }
    if (tombstoned.count(r.id) != 0) return;  // record line after its tombstone: stays dead
    auto it = index_by_id.find(r.id);
    if (it == index_by_id.end()) {
      index_by_id.emplace(r.id, out.size());
      out.push_back(std::move(r));
    } else {
      out[it->second] = std::move(r);  // last line for an id wins
    }
  };

  // Streamed line by line: peak RAM is one line, never the whole file.
  std::string line;
  bool overlong = false;  // skipping the rest of an oversized line
  const bool read_ok = io_.readStream(path_, [&](const char* data, size_t n) {
    size_t pos = 0;
    while (pos < n) {
      const char* nl = static_cast<const char*>(std::memchr(data + pos, '\n', n - pos));
      const size_t take = (nl != nullptr ? static_cast<size_t>(nl - data) : n) - pos;
      if (!overlong) {
        if (line.size() + take > kMaxLineBytes) {
          overlong = true;
          line.clear();
          line.shrink_to_fit();
        } else {
          line.append(data + pos, take);
        }
      }
      pos += take;
      if (nl == nullptr) break;
      ++pos;  // consume '\n'
      if (overlong) {
        ++total_lines_;
        ++malformed_out;
        overlong = false;
      } else {
        apply_line(line);
      }
      line.clear();
    }
    return true;
  });
  if (!read_ok) {
    out.clear();
    total_lines_ = 0;
    dead_lines_ = 0;
    malformed_out = 0;
    return 0;
  }
  // Torn tail (no trailing newline): applied like any other line.
  if (overlong) {
    ++total_lines_;
    ++malformed_out;
  } else {
    apply_line(line);
  }

  if (!tombstoned.empty()) {
    // Records loaded before their tombstone line are removed here.
    out.erase(std::remove_if(out.begin(), out.end(),
                             [&](const AlertRecord& r) {
                               return tombstoned.count(r.id) != 0;
                             }),
              out.end());
  }
  live_ids_.clear();
  for (const AlertRecord& r : out) live_ids_.insert(r.id);
  dead_lines_ = total_lines_ - live_ids_.size();
  return out.size();
}

bool AlertDb::compact(const std::vector<AlertRecord>& records, std::string& err) {
  return compact(
      records.size(), [&](size_t i) { return &records[i]; }, err);
}

bool AlertDb::compact(size_t count, const RecordAt& at, std::string& err) {
  const std::string tmp = path_ + ".tmp";
  // 1. Stream one line per record into tmp; only one serialized line is alive
  //    at a time. The digest records exactly what was handed to the file.
  StreamDigest written;
  bool missing_record = false;
  const bool wrote = io_.writeStream(tmp, [&](const IAlertFileIo::ChunkSink& sink) {
    std::string line;
    for (size_t i = 0; i < count; ++i) {
      const AlertRecord* r = at(i);
      if (r == nullptr) {
        missing_record = true;
        return false;
      }
      line = recordToJsonText(*r);
      line.push_back('\n');
      written.add(line.data(), line.size());
      if (!sink(line.data(), line.size())) return false;
    }
    return true;
  });
  if (!wrote) {
    ++io_failures_;
    io_.remove(tmp);
    err = missing_record ? "record source changed during compaction" : "write failed: " + tmp;
    return false;
  }
  // 2. Read-back verification in small chunks: byte count, line count and hash
  //    must match what was written before tmp may replace the original.
  StreamDigest readback;
  if (!io_.readStream(tmp, [&](const char* data, size_t n) {
        readback.add(data, n);
        return readback.bytes <= written.bytes;  // longer than written: stop early
      })) {
    ++io_failures_;
    io_.remove(tmp);
    err = "read-back failed: " + tmp;
    return false;
  }
  if (!(readback == written) || written.lines != count) {
    ++io_failures_;
    io_.remove(tmp);
    err = "read-back verify failed: " + tmp;
    return false;
  }
  // 3. Atomic replace (FatFS: eego_hal fsutil::replaceFile via the IO impl).
  if (!io_.rename(tmp, path_)) {
    ++io_failures_;
    io_.remove(tmp);  // rollback: original stays untouched
    err = "rename failed: " + tmp + " -> " + path_;
    return false;
  }
  total_lines_ = count;
  dead_lines_ = 0;
  live_ids_.clear();
  for (size_t i = 0; i < count; ++i) {
    if (const AlertRecord* r = at(i)) live_ids_.insert(r->id);
  }
  return true;
}

}  // namespace alert_core
