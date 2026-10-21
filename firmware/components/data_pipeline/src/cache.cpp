// SPDX-License-Identifier: GPL-3.0-or-later
#include "data_pipeline/cache.h"

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <set>

#include "dashboard_core/json.h"

namespace eego::dashboard::pipeline {
namespace {

bool validId(std::string_view id) {
  return !id.empty() && id.size() <= 64 &&
         std::all_of(id.begin(), id.end(), [](char c) {
           return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-';
         });
}

JsonValue string(const std::string& value) { return JsonValue::makeString(value); }
JsonValue integer(int64_t value) { return JsonValue::makeInt(value); }

JsonValue labelsToJson(const std::vector<KeyValue>& labels) {
  JsonValue::Object object;
  for (const KeyValue& label : labels) object.emplace_back(label.key, string(label.value));
  return JsonValue::makeObject(std::move(object));
}

JsonValue sampleToJson(const MetricSample& sample) {
  JsonValue::Object object;
  object.emplace_back("source_id", string(sample.source_id));
  object.emplace_back("metric", string(sample.metric));
  object.emplace_back("timestamp_ms", integer(sample.timestamp_ms));
  object.emplace_back("value", JsonValue::makeDouble(sample.value));
  object.emplace_back("unit", string(sample.unit));
  object.emplace_back("labels", labelsToJson(sample.labels));
  return JsonValue::makeObject(std::move(object));
}

JsonValue eventToJson(const Event& event) {
  JsonValue::Object object;
  object.emplace_back("id", string(event.id));
  object.emplace_back("rule_id", string(event.rule_id));
  object.emplace_back("source_id", string(event.source_id));
  object.emplace_back("timestamp_ms", integer(event.timestamp_ms));
  object.emplace_back("severity", string(severityName(event.severity)));
  object.emplace_back("state", string(eventStateName(event.state)));
  object.emplace_back("title", string(event.title));
  object.emplace_back("message", string(event.message));
  object.emplace_back("linked_panel_id", string(event.linked_panel_id));
  return JsonValue::makeObject(std::move(object));
}

uint64_t fnv1a(std::string_view text) {
  uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char c : text) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::string hex(uint64_t value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(16, '0');
  for (size_t i = 0; i < 16; ++i) out[i] = kHex[(value >> ((15 - i) * 4)) & 0xf];
  return out;
}

bool readFile(const std::string& path, size_t maximum, std::string& out, std::string& error) {
  struct stat info;
  if (::stat(path.c_str(), &info) != 0 || info.st_size < 0) {
    error = "cache file is unavailable";
    return false;
  }
  if (static_cast<uint64_t>(info.st_size) > maximum) {
    error = "cache file exceeds configured byte budget";
    return false;
  }
  FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    error = "cache file open failed";
    return false;
  }
  out.resize(static_cast<size_t>(info.st_size));
  const size_t read = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), file);
  const bool ok = read == out.size() && !std::ferror(file);
  std::fclose(file);
  out.resize(read);
  if (!ok) {
    error = "cache file read failed";
    return false;
  }
  return true;
}

bool writeAtomic(const std::string& path, const char* data, size_t size, std::string& error) {
  const std::string temporary = path + ".tmp";
  FILE* file = std::fopen(temporary.c_str(), "wb");
  if (file == nullptr) {
    error = "cache temporary file open failed";
    return false;
  }
  const bool written = size == 0 || std::fwrite(data, 1, size, file) == size;
  const bool flushed = std::fflush(file) == 0;
  const bool closed = std::fclose(file) == 0;
  if (!written || !flushed || !closed) {
    std::remove(temporary.c_str());
    error = "cache write failed";
    return false;
  }
  if (std::rename(temporary.c_str(), path.c_str()) != 0) {
    std::remove(temporary.c_str());
    error = "cache atomic rename failed";
    return false;
  }
  return true;
}

bool writeAtomic(const std::string& path, const std::string& data, std::string& error) {
  return writeAtomic(path, data.data(), data.size(), error);
}

bool readLabels(const JsonValue& value, std::vector<KeyValue>& out) {
  if (value.type() != JsonValue::Type::Object || value.objectItems().size() > kMaxLabels)
    return false;
  out.clear();
  for (const auto& member : value.objectItems()) {
    if (member.first.empty() || member.first.size() > 64 ||
        member.second.type() != JsonValue::Type::String ||
        jsonCodePointCount(member.second.stringValue()) > 512)
      return false;
    out.push_back({member.first, member.second.stringValue()});
  }
  return true;
}

bool readSample(const JsonValue& value, MetricSample& out) {
  if (value.type() != JsonValue::Type::Object) return false;
  const JsonValue* source = value.find("source_id");
  const JsonValue* metric = value.find("metric");
  const JsonValue* timestamp = value.find("timestamp_ms");
  const JsonValue* number = value.find("value");
  const JsonValue* unit = value.find("unit");
  const JsonValue* labels = value.find("labels");
  if (source == nullptr || source->type() != JsonValue::Type::String ||
      metric == nullptr || metric->type() != JsonValue::Type::String ||
      timestamp == nullptr || timestamp->type() != JsonValue::Type::Int ||
      timestamp->intValue() < 0 || number == nullptr ||
      (number->type() != JsonValue::Type::Int && number->type() != JsonValue::Type::Double) ||
      !std::isfinite(number->doubleValue()) || unit == nullptr ||
      unit->type() != JsonValue::Type::String || labels == nullptr)
    return false;
  out.source_id = source->stringValue();
  out.metric = metric->stringValue();
  out.timestamp_ms = timestamp->intValue();
  out.value = number->doubleValue();
  out.unit = unit->stringValue();
  return validId(out.source_id) && !out.metric.empty() && out.metric.size() <= 128 &&
         out.unit.size() <= 32 && readLabels(*labels, out.labels);
}

bool readEvent(const JsonValue& value, Event& out) {
  if (value.type() != JsonValue::Type::Object) return false;
  const JsonValue* id = value.find("id");
  const JsonValue* rule = value.find("rule_id");
  const JsonValue* source = value.find("source_id");
  const JsonValue* timestamp = value.find("timestamp_ms");
  const JsonValue* severity = value.find("severity");
  const JsonValue* state = value.find("state");
  const JsonValue* title = value.find("title");
  const JsonValue* message = value.find("message");
  const JsonValue* linked = value.find("linked_panel_id");
  if (id == nullptr || id->type() != JsonValue::Type::String ||
      rule == nullptr || rule->type() != JsonValue::Type::String ||
      source == nullptr || source->type() != JsonValue::Type::String ||
      timestamp == nullptr || timestamp->type() != JsonValue::Type::Int ||
      timestamp->intValue() < 0 || severity == nullptr ||
      severity->type() != JsonValue::Type::String || state == nullptr ||
      state->type() != JsonValue::Type::String || title == nullptr ||
      title->type() != JsonValue::Type::String || message == nullptr ||
      message->type() != JsonValue::Type::String || linked == nullptr ||
      linked->type() != JsonValue::Type::String)
    return false;
  out.id = id->stringValue();
  out.rule_id = rule->stringValue();
  out.source_id = source->stringValue();
  out.timestamp_ms = timestamp->intValue();
  out.title = title->stringValue();
  out.message = message->stringValue();
  out.linked_panel_id = linked->stringValue();
  return validId(out.id) && validId(out.source_id) &&
         severityFromString(severity->stringValue(), out.severity) &&
         eventStateFromString(state->stringValue(), out.state) &&
         jsonCodePointCount(out.title) <= kMaxTitleCodePoints &&
         jsonCodePointCount(out.message) <= kMaxTextCodePoints &&
         jsonCodePointCount(out.rule_id) <= 64 && jsonCodePointCount(out.linked_panel_id) <= 64;
}

bool historyEntryFromJson(const JsonValue& root, HistoryEntry& out) {
  const JsonValue* sample = root.find("sample");
  const JsonValue* reset = root.find("reset_at");
  if (sample == nullptr || reset == nullptr || reset->type() != JsonValue::Type::Int ||
      reset->intValue() < 0 || !readSample(*sample, out.sample))
    return false;
  out.reset_at_ms = reset->intValue();
  return true;
}

std::string historyEntryToJson(const HistoryEntry& entry) {
  JsonValue::Object object;
  object.emplace_back("sample", sampleToJson(entry.sample));
  object.emplace_back("reset_at", integer(entry.reset_at_ms));
  return JsonValue::makeObject(std::move(object)).toString();
}

std::vector<HistoryEntry> boundHistory(const std::vector<HistoryEntry>& input,
                                       const CacheLimits& limits, std::string& error) {
  SourceResult result;
  result.source_id = input.empty() ? "cache" : input.front().sample.source_id;
  result.metrics.reserve(input.size());
  for (const HistoryEntry& entry : input) {
    result.metrics.push_back(entry.sample);
    if (entry.reset_at_ms > 0) result.reset_at_ms[entry.sample.metric] = entry.reset_at_ms;
  }
  HistoryLimits history_limits;
  history_limits.max_series = std::max<size_t>(limits.max_history_points, 1);
  history_limits.max_points_per_series = std::max<size_t>(limits.max_history_points, 2);
  history_limits.downsample_points = std::max<size_t>(limits.downsample_points, 1);
  history_limits.max_total_points = std::max<size_t>(limits.max_history_points, 1);
  history_limits.retention_ms = std::numeric_limits<int64_t>::max();
  MetricHistory history(history_limits);
  if (!input.empty() && !history.append(result, 0, error)) return {};
  return history.entries();
}

std::string serializeHistory(std::vector<HistoryEntry> entries, size_t max_bytes) {
  std::stable_sort(entries.begin(), entries.end(), [](const HistoryEntry& left,
                                                      const HistoryEntry& right) {
    return left.sample.timestamp_ms < right.sample.timestamp_ms;
  });
  std::vector<std::string> lines;
  lines.reserve(entries.size());
  size_t total = 0;
  for (const HistoryEntry& entry : entries) {
    lines.push_back(historyEntryToJson(entry) + "\n");
    total += lines.back().size();
  }
  size_t first = 0;
  while (total > max_bytes && first < lines.size()) {
    total -= lines[first].size();
    ++first;
  }
  std::string out;
  out.reserve(total);
  for (size_t i = first; i < lines.size(); ++i) out += lines[i];
  return out;
}

}  // namespace

SdCache::SdCache(std::string directory, CacheLimits limits)
    : directory_(std::move(directory)), limits_(limits) {
  if (!directory_.empty() && directory_.back() == '/') directory_.pop_back();
  limits_.max_history_points = std::max<size_t>(limits_.max_history_points, 1);
  limits_.downsample_points = std::max<size_t>(limits_.downsample_points, 1);
  limits_.max_history_bytes = std::max<size_t>(limits_.max_history_bytes, 1024);
  limits_.max_snapshot_bytes = std::max<size_t>(limits_.max_snapshot_bytes, 1024);
  limits_.max_image_bytes = std::min<size_t>(limits_.max_image_bytes, kMaxImageBytes);
}

std::string SdCache::historyPath(const std::string& source_id) const {
  return directory_ + "/" + source_id + ".history.jsonl";
}
std::string SdCache::snapshotPath(const std::string& source_id) const {
  return directory_ + "/" + source_id + ".snapshot.json";
}
std::string SdCache::imagePath(const std::string& source_id) const {
  return directory_ + "/" + source_id + ".image";
}

bool SdCache::appendHistory(const std::string& source_id,
                            const std::vector<HistoryEntry>& entries, std::string& error) {
  error.clear();
  if (!validId(source_id)) {
    error = "invalid cache source id";
    return false;
  }
  for (const HistoryEntry& entry : entries) {
    if (entry.sample.source_id != source_id) {
      error = "history entry source does not match cache source";
      return false;
    }
  }
  struct stat info;
  const bool exists = ::stat(historyPath(source_id).c_str(), &info) == 0;
  std::vector<HistoryEntry> combined;
  if (exists && !loadHistory(source_id, combined, error)) return false;
  combined.insert(combined.end(), entries.begin(), entries.end());
  combined = boundHistory(combined, limits_, error);
  if (!error.empty()) return false;
  const std::string data = serializeHistory(std::move(combined), limits_.max_history_bytes);
  return writeAtomic(historyPath(source_id), data, error);
}

bool SdCache::loadHistory(const std::string& source_id, std::vector<HistoryEntry>& out,
                          std::string& error) const {
  out.clear();
  error.clear();
  if (!validId(source_id)) {
    error = "invalid cache source id";
    return false;
  }
  std::string data;
  if (!readFile(historyPath(source_id), limits_.max_history_bytes, data, error)) return false;
  size_t position = 0;
  size_t valid_records = 0;
  while (position < data.size()) {
    size_t end = data.find('\n', position);
    const bool complete = end != std::string::npos;
    if (!complete) end = data.size();
    const std::string_view line(data.data() + position, end - position);
    position = end + 1;
    if (line.empty()) continue;
    JsonValue root;
    JsonParseError parse_error;
    HistoryEntry entry;
    if (!complete || !jsonParse(line, root, parse_error) ||
        !historyEntryFromJson(root, entry) || entry.sample.source_id != source_id) {
      continue;
    }
    out.push_back(std::move(entry));
    if (++valid_records > limits_.max_history_points * 2) {
      error = "history record count exceeds load budget";
      out.clear();
      return false;
    }
  }
  if (out.empty() && !data.empty()) {
    error = "history contains no valid records";
    return false;
  }
  out = boundHistory(out, limits_, error);
  return error.empty();
}

bool SdCache::writeSnapshot(const SourceResult& result, std::string& error) {
  error.clear();
  if (!validId(result.source_id) || result.metrics.size() > kMaxResultMetrics ||
      result.events.size() > kMaxResultEvents || result.fetched_at_ms < 0) {
    error = "invalid snapshot source or collection budget";
    return false;
  }
  if (result.has_image) {
    if (result.image.bytes.empty() || result.image.bytes.size() > limits_.max_image_bytes) {
      error = "snapshot image exceeds configured budget";
      return false;
    }
    const std::string bytes(result.image.bytes.begin(), result.image.bytes.end());
    if (!writeAtomic(imagePath(result.source_id), bytes, error)) return false;
  }
  JsonValue::Object root;
  root.emplace_back("version", integer(1));
  root.emplace_back("source_id", string(result.source_id));
  root.emplace_back("fetched_at_ms", integer(result.fetched_at_ms));
  JsonValue::Object resets;
  for (const auto& reset : result.reset_at_ms) {
    if (reset.second < 0) {
      error = "snapshot reset_at cannot be negative";
      return false;
    }
    resets.emplace_back(reset.first, integer(reset.second));
  }
  root.emplace_back("reset_at", JsonValue::makeObject(std::move(resets)));
  JsonValue::Array metrics;
  for (const MetricSample& sample : result.metrics) metrics.push_back(sampleToJson(sample));
  root.emplace_back("metrics", JsonValue::makeArray(std::move(metrics)));
  JsonValue::Array events;
  for (const Event& event : result.events) events.push_back(eventToJson(event));
  root.emplace_back("events", JsonValue::makeArray(std::move(events)));
  root.emplace_back("text", result.has_text ? string(result.text.text) : JsonValue());
  if (result.has_image) {
    const std::string bytes(result.image.bytes.begin(), result.image.bytes.end());
    JsonValue::Object image;
    image.emplace_back("media_type", string(result.image.media_type));
    image.emplace_back("size", integer(static_cast<int64_t>(bytes.size())));
    image.emplace_back("digest", string(hex(fnv1a(bytes))));
    root.emplace_back("image", JsonValue::makeObject(std::move(image)));
  } else {
    root.emplace_back("image", JsonValue());
  }
  const std::string data = JsonValue::makeObject(std::move(root)).toString();
  if (data.size() > limits_.max_snapshot_bytes) {
    error = "snapshot exceeds configured byte budget";
    return false;
  }
  if (!writeAtomic(snapshotPath(result.source_id), data, error)) return false;
  if (!result.has_image) std::remove(imagePath(result.source_id).c_str());
  return true;
}

bool SdCache::readSnapshot(const std::string& source_id, SourceResult& out,
                           std::string& error) const {
  out = SourceResult{};
  error.clear();
  if (!validId(source_id)) {
    error = "invalid cache source id";
    return false;
  }
  std::string data;
  if (!readFile(snapshotPath(source_id), limits_.max_snapshot_bytes, data, error)) return false;
  JsonValue root;
  JsonParseError parse_error;
  if (!jsonParse(data, root, parse_error) || root.type() != JsonValue::Type::Object) {
    error = "snapshot is not valid bounded JSON";
    return false;
  }
  const JsonValue* version = root.find("version");
  const JsonValue* stored_id = root.find("source_id");
  const JsonValue* fetched = root.find("fetched_at_ms");
  const JsonValue* resets = root.find("reset_at");
  const JsonValue* metrics = root.find("metrics");
  const JsonValue* events = root.find("events");
  const JsonValue* text = root.find("text");
  const JsonValue* image = root.find("image");
  if (version == nullptr || version->type() != JsonValue::Type::Int ||
      version->intValue() != 1 || stored_id == nullptr ||
      stored_id->type() != JsonValue::Type::String || stored_id->stringValue() != source_id ||
      fetched == nullptr || fetched->type() != JsonValue::Type::Int ||
      fetched->intValue() < 0 || resets == nullptr ||
      resets->type() != JsonValue::Type::Object || metrics == nullptr ||
      metrics->type() != JsonValue::Type::Array ||
      metrics->arrayItems().size() > kMaxResultMetrics ||
      events == nullptr || events->type() != JsonValue::Type::Array ||
      events->arrayItems().size() > kMaxResultEvents || text == nullptr || image == nullptr) {
    error = "snapshot envelope is invalid or unsupported";
    return false;
  }
  out.source_id = source_id;
  out.fetched_at_ms = fetched->intValue();
  for (const auto& reset : resets->objectItems()) {
    if (reset.second.type() != JsonValue::Type::Int || reset.second.intValue() < 0) {
      error = "snapshot contains an invalid reset_at";
      out = SourceResult{};
      return false;
    }
    out.reset_at_ms[reset.first] = reset.second.intValue();
  }
  for (const JsonValue& value : metrics->arrayItems()) {
    MetricSample sample;
    if (!readSample(value, sample) || sample.source_id != source_id) {
      error = "snapshot contains an invalid metric";
      out = SourceResult{};
      return false;
    }
    out.metrics.push_back(std::move(sample));
  }
  for (const JsonValue& value : events->arrayItems()) {
    Event event;
    if (!readEvent(value, event) || event.source_id != source_id) {
      error = "snapshot contains an invalid event";
      out = SourceResult{};
      return false;
    }
    out.events.push_back(std::move(event));
  }
  if (!text->isNull()) {
    if (text->type() != JsonValue::Type::String ||
        jsonCodePointCount(text->stringValue()) > kMaxTextCodePoints) {
      error = "snapshot text is invalid";
      out = SourceResult{};
      return false;
    }
    out.has_text = true;
    out.text.text = text->stringValue();
    out.text.fetched_at_ms = out.fetched_at_ms;
  }
  if (!image->isNull()) {
    const JsonValue* media = image->find("media_type");
    const JsonValue* size = image->find("size");
    const JsonValue* digest = image->find("digest");
    if (media == nullptr || media->type() != JsonValue::Type::String || size == nullptr ||
        size->type() != JsonValue::Type::Int || size->intValue() <= 0 ||
        size->intValue() > static_cast<int64_t>(limits_.max_image_bytes) || digest == nullptr ||
        digest->type() != JsonValue::Type::String) {
      error = "snapshot image metadata is invalid";
      out = SourceResult{};
      return false;
    }
    std::string bytes;
    if (!readFile(imagePath(source_id), limits_.max_image_bytes, bytes, error) ||
        bytes.size() != static_cast<size_t>(size->intValue()) ||
        hex(fnv1a(bytes)) != digest->stringValue()) {
      if (error.empty()) error = "snapshot image is missing or does not match metadata";
      out = SourceResult{};
      return false;
    }
    out.has_image = true;
    out.image.media_type = media->stringValue();
    out.image.bytes.assign(bytes.begin(), bytes.end());
    out.image.fetched_at_ms = out.fetched_at_ms;
  }
  return true;
}

}  // namespace eego::dashboard::pipeline
