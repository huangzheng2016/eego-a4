// SPDX-License-Identifier: GPL-3.0-or-later
#include "alert_core/json_writer.h"

namespace eego::json {

ObjectWriter::ObjectWriter(size_t reserve) { object_.reserve(reserve); }

ObjectWriter& ObjectWriter::integer(const char* key, int64_t value) {
  object_.emplace_back(key, JsonValue::makeInt(value));
  return *this;
}

ObjectWriter& ObjectWriter::number(const char* key, double value) {
  object_.emplace_back(key, JsonValue::makeDouble(value));
  return *this;
}

ObjectWriter& ObjectWriter::boolean(const char* key, bool value) {
  object_.emplace_back(key, JsonValue::makeBool(value));
  return *this;
}

ObjectWriter& ObjectWriter::string(const char* key, const char* value) {
  object_.emplace_back(key, JsonValue::makeString(value != nullptr ? value : ""));
  return *this;
}

ObjectWriter& ObjectWriter::string(const char* key, const std::string& value) {
  object_.emplace_back(key, JsonValue::makeString(value));
  return *this;
}

ObjectWriter& ObjectWriter::value(const char* key, JsonValue&& value) {
  object_.emplace_back(key, std::move(value));
  return *this;
}

ObjectWriter& ObjectWriter::null(const char* key) {
  object_.emplace_back(key, JsonValue::makeNull());
  return *this;
}

JsonValue ObjectWriter::take() { return JsonValue::makeObject(std::move(object_)); }

}  // namespace eego::json
