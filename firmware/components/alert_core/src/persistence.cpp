// SPDX-License-Identifier: GPL-3.0-or-later
#include "alert_core/persistence.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "alert_core/json.h"

namespace alert_core {
namespace {

bool sanitizeSourceId(const std::string& id) {
  if (id.empty() || id.size() > 64) return false;
  for (char c : id) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

std::string cachePath(const std::string& dir, const std::string& source_id) {
  std::string base = dir;
  if (!base.empty() && base.back() != '/') base.push_back('/');
  return base + source_id + ".json";
}

}  // namespace

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

size_t AlertDb::load(std::vector<AlertRecord>& out, size_t& malformed_out) {
  out.clear();
  malformed_out = 0;
  total_lines_ = 0;
  dead_lines_ = 0;

  std::string text;
  if (!io_.readAll(path_, text)) return 0;

  size_t pos = 0;
  std::unordered_map<std::string, size_t> index_by_id;
  std::unordered_set<std::string> tombstoned;
  while (pos <= text.size()) {
    size_t eol = text.find('\n', pos);
    if (eol == std::string::npos) eol = text.size();
    std::string line = text.substr(pos, eol - pos);
    pos = eol + 1;
    if (line.empty()) {
      if (eol == text.size()) break;
      continue;
    }
    ++total_lines_;
    JsonValue v;
    JsonParseError perr;
    if (!jsonParse(line, v, perr)) {
      ++malformed_out;
      if (eol == text.size()) break;
      continue;
    }
    if (const JsonValue* t = v.find("tombstone");
        t != nullptr && t->type() == JsonValue::Type::String && !t->stringValue().empty()) {
      tombstoned.insert(t->stringValue());
      if (eol == text.size()) break;
      continue;
    }
    AlertRecord r;
    if (!recordFromJson(v, r)) {
      ++malformed_out;
      if (eol == text.size()) break;
      continue;
    }
    if (tombstoned.count(r.id) != 0) {
      if (eol == text.size()) break;
      continue;  // record line after its tombstone: stays dead
    }
    auto it = index_by_id.find(r.id);
    if (it == index_by_id.end()) {
      index_by_id.emplace(r.id, out.size());
      out.push_back(std::move(r));
    } else {
      out[it->second] = std::move(r);  // last line for an id wins
    }
    if (eol == text.size()) break;
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
  std::string data;
  for (const AlertRecord& r : records) {
    data += recordToJsonText(r);
    data.push_back('\n');
  }
  const std::string tmp = path_ + ".tmp";
  if (!io_.writeAll(tmp, data)) {
    ++io_failures_;
    io_.remove(tmp);
    err = "write failed: " + tmp;
    return false;
  }
  // Read-back verification: the rewritten file must parse back to exactly the
  // same id sequence before it is allowed to replace the original.
  std::string verify;
  if (!io_.readAll(tmp, verify)) {
    ++io_failures_;
    io_.remove(tmp);
    err = "read-back failed: " + tmp;
    return false;
  }
  if (verify != data) {
    ++io_failures_;
    io_.remove(tmp);
    err = "read-back mismatch: " + tmp;
    return false;
  }
  size_t lines = 0;
  size_t pos = 0;
  bool mismatch = false;
  while (pos < verify.size() && lines < records.size()) {
    size_t eol = verify.find('\n', pos);
    if (eol == std::string::npos) break;
    JsonValue v;
    JsonParseError perr;
    AlertRecord r;
    if (!jsonParse(verify.substr(pos, eol - pos), v, perr) || !recordFromJson(v, r) ||
        r.id != records[lines].id) {
      mismatch = true;
      break;
    }
    ++lines;
    pos = eol + 1;
  }
  if (mismatch || lines != records.size()) {
    ++io_failures_;
    io_.remove(tmp);
    err = "read-back verify failed: " + tmp;
    return false;
  }
  if (!io_.rename(tmp, path_)) {
    ++io_failures_;
    io_.remove(tmp);  // rollback: original stays untouched
    err = "rename failed: " + tmp + " -> " + path_;
    return false;
  }
  total_lines_ = records.size();
  dead_lines_ = 0;
  live_ids_.clear();
  for (const AlertRecord& r : records) live_ids_.insert(r.id);
  return true;
}

bool writeSourceCache(IAlertFileIo& io, const std::string& dir, const std::string& source_id,
                      const std::string& body, std::string& err) {
  if (!sanitizeSourceId(source_id)) {
    err = "invalid source id";
    return false;
  }
  // Only cache parseable JSON — the file must always be usable offline.
  JsonValue v;
  JsonParseError perr;
  if (!jsonParse(body, v, perr)) {
    err = "cache body is not valid JSON";
    return false;
  }
  const std::string path = cachePath(dir, source_id);
  const std::string tmp = path + ".tmp";
  if (!io.writeAll(tmp, body) || !io.rename(tmp, path)) {
    io.remove(tmp);
    err = "cache write failed: " + path;
    return false;
  }
  return true;
}

bool readSourceCache(IAlertFileIo& io, const std::string& dir, const std::string& source_id,
                     std::string& body_out) {
  if (!sanitizeSourceId(source_id)) return false;
  return io.readAll(cachePath(dir, source_id), body_out);
}

bool removeSourceCache(IAlertFileIo& io, const std::string& dir, const std::string& source_id) {
  if (!sanitizeSourceId(source_id)) return false;
  return io.remove(cachePath(dir, source_id));
}

}  // namespace alert_core
