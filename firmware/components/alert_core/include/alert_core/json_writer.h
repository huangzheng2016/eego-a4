// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Builds a JSON object one member per call. Every add method is out of line, so the
// caller's stack frame only holds scalar arguments. An initializer list
// (`makeObject({{"a", ...}, {"b", ...}})`) keeps every pair and value temporary alive at
// once, which made some response builders need 5-24 KB of stack.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "json_dom/json.h"

namespace eego::json {

class ObjectWriter {
 public:
  explicit ObjectWriter(size_t reserve = 0);

  __attribute__((noinline)) ObjectWriter& integer(const char* key, int64_t value);
  __attribute__((noinline)) ObjectWriter& number(const char* key, double value);
  __attribute__((noinline)) ObjectWriter& boolean(const char* key, bool value);
  __attribute__((noinline)) ObjectWriter& string(const char* key, const char* value);
  __attribute__((noinline)) ObjectWriter& string(const char* key, const std::string& value);
  __attribute__((noinline)) ObjectWriter& value(const char* key, JsonValue&& value);
  __attribute__((noinline)) ObjectWriter& null(const char* key);

  __attribute__((noinline)) JsonValue take();
  JsonValue::Object& members() { return object_; }

 private:
  JsonValue::Object object_;
};

}  // namespace eego::json
