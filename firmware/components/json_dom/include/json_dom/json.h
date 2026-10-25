// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Minimal DOM-style JSON value, strict parser and canonical writer shared by
// alert_core, dashboard_core and everything above them. Pure C++ (no ESP-IDF,
// no cJSON) so it builds unchanged for the host tests and the firmware.
// Objects keep insertion order, so serialization is deterministic.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eego::json {

class JsonValue {
 public:
  enum class Type { Null, Bool, Int, Double, String, Array, Object };
  using Array = std::vector<JsonValue>;
  using Object = std::vector<std::pair<std::string, JsonValue>>;

  JsonValue() = default;
  static JsonValue makeNull();
  static JsonValue makeBool(bool value);
  static JsonValue makeInt(int64_t value);
  static JsonValue makeDouble(double value);
  static JsonValue makeString(std::string value);
  static JsonValue makeArray(Array value);
  static JsonValue makeObject(Object value);

  Type type() const { return type_; }
  bool isNull() const { return type_ == Type::Null; }
  bool isScalar() const { return type_ != Type::Array && type_ != Type::Object; }

  bool boolValue(bool fallback = false) const;
  int64_t intValue(int64_t fallback = 0) const;
  double doubleValue(double fallback = 0.0) const;
  const std::string& stringValue() const;
  const Array& arrayItems() const;
  Array& arrayItems();
  const Object& objectItems() const;
  Object& objectItems();
  const JsonValue* find(std::string_view key) const;
  JsonValue* find(std::string_view key);

  std::string toString() const;
  std::string scalarText() const;

 private:
  Type type_ = Type::Null;
  bool bool_ = false;
  int64_t int_ = 0;
  double double_ = 0.0;
  std::string string_;
  Array array_;
  Object object_;
};

struct JsonParseError {
  size_t offset = 0;
  std::string message;
};

// Strict RFC 8259: duplicate keys, invalid Unicode escapes, non-finite numbers,
// trailing content, and nesting beyond max_depth are rejected.
bool jsonParse(std::string_view text, JsonValue& out, JsonParseError& err,
               size_t max_depth = 16);
bool jsonIsValidUtf8(std::string_view text);
// Returns std::string_view::npos for invalid UTF-8.
size_t jsonCodePointCount(std::string_view text);
std::string jsonEscape(std::string_view text);

}  // namespace eego::json
