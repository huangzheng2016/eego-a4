// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Minimal DOM-style JSON value, parser and writer.
//
// alert_core is a pure C++ component (M12): it must not depend on ESP-IDF,
// cJSON or any other framework JSON library, so it carries its own small
// RFC 8259 implementation. Strict mode only — no comments, no trailing
// commas, no trailing garbage, nesting depth capped. Parsing errors report a
// byte offset and message so the ingest layer can turn them into field-level
// 400 responses (error envelope itself belongs to the admin layer, M13).

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace alert_core {

class JsonValue {
 public:
  enum class Type { Null, Bool, Int, Double, String, Array, Object };
  using Array = std::vector<JsonValue>;
  // Insertion-ordered object: deterministic serialization without a map.
  using Object = std::vector<std::pair<std::string, JsonValue>>;

  JsonValue() = default;
  static JsonValue makeNull();
  static JsonValue makeBool(bool v);
  static JsonValue makeInt(int64_t v);
  static JsonValue makeDouble(double v);
  static JsonValue makeString(std::string v);
  static JsonValue makeArray(Array v);
  static JsonValue makeObject(Object v);

  Type type() const { return type_; }
  bool isNull() const { return type_ == Type::Null; }
  bool isScalar() const { return type_ != Type::Array && type_ != Type::Object; }

  bool boolValue(bool fallback = false) const;
  int64_t intValue(int64_t fallback = 0) const;
  double doubleValue(double fallback = 0.0) const;
  const std::string& stringValue() const;
  const Array& arrayItems() const;
  const Object& objectItems() const;

  // Object member lookup; nullptr when this is not an object or key is absent.
  const JsonValue* find(std::string_view key) const;

  // Canonical compact serialization (insertion key order, minimal escapes).
  std::string toString() const;

  // Canonical text of a scalar: strings as-is, ints/doubles as number text,
  // bool as "true"/"false", null as "null". Empty string for non-scalars.
  // Used for dedup-key values and field length validation.
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

// Parses exactly one JSON document. Returns false and fills `err` on any
// syntax error, excess depth (>16) or trailing garbage after the value.
bool jsonParse(const std::string& text, JsonValue& out, JsonParseError& err);

// Escapes `s` as a JSON string literal body (no surrounding quotes).
std::string jsonEscape(std::string_view s);

}  // namespace alert_core
