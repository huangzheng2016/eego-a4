// SPDX-License-Identifier: GPL-3.0-or-later
#include "json_dom/json.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace eego::json {
namespace {

const JsonValue::Array kEmptyArray;
const JsonValue::Object kEmptyObject;
const std::string kEmptyString;

void writeString(std::string& out, std::string_view value) {
  out.push_back('"');
  out += jsonEscape(value);
  out.push_back('"');
}

void writeValue(std::string& out, const JsonValue& value) {
  switch (value.type()) {
    case JsonValue::Type::Null:
      out += "null";
      break;
    case JsonValue::Type::Bool:
      out += value.boolValue() ? "true" : "false";
      break;
    case JsonValue::Type::Int:
      out += std::to_string(value.intValue());
      break;
    case JsonValue::Type::Double: {
      const double number = value.doubleValue();
      if (!std::isfinite(number)) {
        out += "null";
      } else if (number >= -9223372036854775808.0 && number < 9223372036854775808.0 &&
                 number == static_cast<int64_t>(number)) {
        out += std::to_string(static_cast<int64_t>(number));
      } else {
        char buffer[40];
        std::snprintf(buffer, sizeof(buffer), "%.17g", number);
        out += buffer;
      }
      break;
    }
    case JsonValue::Type::String:
      writeString(out, value.stringValue());
      break;
    case JsonValue::Type::Array:
      out.push_back('[');
      for (size_t i = 0; i < value.arrayItems().size(); ++i) {
        if (i != 0) out.push_back(',');
        writeValue(out, value.arrayItems()[i]);
      }
      out.push_back(']');
      break;
    case JsonValue::Type::Object:
      out.push_back('{');
      for (size_t i = 0; i < value.objectItems().size(); ++i) {
        if (i != 0) out.push_back(',');
        writeString(out, value.objectItems()[i].first);
        out.push_back(':');
        writeValue(out, value.objectItems()[i].second);
      }
      out.push_back('}');
      break;
  }
}

void appendUtf8(std::string& out, uint32_t cp) {
  if (cp <= 0x7f) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7ff) {
    out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  } else if (cp <= 0xffff) {
    out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  } else {
    out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
  }
}

class Parser {
 public:
  Parser(std::string_view text, size_t max_depth) : text_(text), max_depth_(max_depth) {}

  bool parse(JsonValue& out) {
    skipWhitespace();
    if (!parseValue(out, 0)) return false;
    skipWhitespace();
    if (position_ != text_.size()) return fail("trailing content after JSON value");
    return true;
  }

  const JsonParseError& error() const { return error_; }

 private:
  bool fail(const char* message) {
    if (error_.message.empty()) {
      error_.offset = position_;
      error_.message = message;
    }
    return false;
  }

  void skipWhitespace() {
    while (position_ < text_.size()) {
      const char c = text_[position_];
      if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
      ++position_;
    }
  }

  bool literal(std::string_view value) {
    if (text_.substr(position_, value.size()) != value) return fail("invalid literal");
    position_ += value.size();
    return true;
  }

  bool parseValue(JsonValue& out, size_t depth) {
    if (depth > max_depth_) return fail("JSON nesting exceeds configured depth");
    if (position_ >= text_.size()) return fail("unexpected end of input");
    switch (text_[position_]) {
      case 'n':
        if (!literal("null")) return false;
        out = JsonValue::makeNull();
        return true;
      case 't':
        if (!literal("true")) return false;
        out = JsonValue::makeBool(true);
        return true;
      case 'f':
        if (!literal("false")) return false;
        out = JsonValue::makeBool(false);
        return true;
      case '"': {
        std::string value;
        if (!parseString(value)) return false;
        out = JsonValue::makeString(std::move(value));
        return true;
      }
      case '[':
        return parseArray(out, depth);
      case '{':
        return parseObject(out, depth);
      default:
        return parseNumber(out);
    }
  }

  bool parseArray(JsonValue& out, size_t depth) {
    ++position_;
    JsonValue::Array items;
    skipWhitespace();
    if (position_ < text_.size() && text_[position_] == ']') {
      ++position_;
      out = JsonValue::makeArray(std::move(items));
      return true;
    }
    for (;;) {
      skipWhitespace();
      JsonValue item;
      if (!parseValue(item, depth + 1)) return false;
      items.push_back(std::move(item));
      skipWhitespace();
      if (position_ >= text_.size()) return fail("unterminated array");
      if (text_[position_] == ']') {
        ++position_;
        out = JsonValue::makeArray(std::move(items));
        return true;
      }
      if (text_[position_] != ',') return fail("expected ',' or ']' in array");
      ++position_;
    }
  }

  bool parseObject(JsonValue& out, size_t depth) {
    ++position_;
    JsonValue::Object members;
    skipWhitespace();
    if (position_ < text_.size() && text_[position_] == '}') {
      ++position_;
      out = JsonValue::makeObject(std::move(members));
      return true;
    }
    for (;;) {
      skipWhitespace();
      if (position_ >= text_.size() || text_[position_] != '"') return fail("expected object key");
      std::string key;
      if (!parseString(key)) return false;
      for (const auto& member : members) {
        if (member.first == key) return fail("duplicate object key");
      }
      skipWhitespace();
      if (position_ >= text_.size() || text_[position_] != ':') return fail("expected ':' after key");
      ++position_;
      skipWhitespace();
      JsonValue value;
      if (!parseValue(value, depth + 1)) return false;
      members.emplace_back(std::move(key), std::move(value));
      skipWhitespace();
      if (position_ >= text_.size()) return fail("unterminated object");
      if (text_[position_] == '}') {
        ++position_;
        out = JsonValue::makeObject(std::move(members));
        return true;
      }
      if (text_[position_] != ',') return fail("expected ',' or '}' in object");
      ++position_;
    }
  }

  bool parseHex4(uint32_t& cp) {
    if (position_ + 4 > text_.size()) return fail("truncated Unicode escape");
    cp = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[position_ + i];
      cp <<= 4;
      if (c >= '0' && c <= '9') {
        cp |= static_cast<uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        cp |= static_cast<uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        cp |= static_cast<uint32_t>(c - 'A' + 10);
      } else {
        return fail("invalid Unicode escape");
      }
    }
    position_ += 4;
    return true;
  }

  bool appendRawUtf8(std::string& out) {
    const size_t start = position_;
    const unsigned char first = static_cast<unsigned char>(text_[position_++]);
    size_t length = 0;
    uint32_t cp = 0;
    uint32_t minimum = 0;
    if (first >= 0xc2 && first <= 0xdf) {
      length = 2;
      cp = first & 0x1f;
      minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
      length = 3;
      cp = first & 0x0f;
      minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
      length = 4;
      cp = first & 0x07;
      minimum = 0x10000;
    } else {
      return fail("invalid UTF-8 in string");
    }
    if (start + length > text_.size()) return fail("truncated UTF-8 in string");
    for (size_t i = 1; i < length; ++i) {
      const unsigned char next = static_cast<unsigned char>(text_[position_++]);
      if ((next & 0xc0) != 0x80) return fail("invalid UTF-8 continuation");
      cp = (cp << 6) | (next & 0x3f);
    }
    if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
      return fail("invalid UTF-8 code point");
    out.append(text_.substr(start, length));
    return true;
  }

  bool parseString(std::string& out) {
    ++position_;
    while (position_ < text_.size()) {
      const unsigned char c = static_cast<unsigned char>(text_[position_]);
      if (c == '"') {
        ++position_;
        return true;
      }
      if (c < 0x20) return fail("unescaped control character in string");
      if (c >= 0x80) {
        if (!appendRawUtf8(out)) return false;
        continue;
      }
      ++position_;
      if (c != '\\') {
        out.push_back(static_cast<char>(c));
        continue;
      }
      if (position_ >= text_.size()) return fail("truncated escape");
      const char escaped = text_[position_++];
      switch (escaped) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          uint32_t cp = 0;
          if (!parseHex4(cp)) return false;
          if (cp >= 0xd800 && cp <= 0xdbff) {
            if (position_ + 2 > text_.size() || text_[position_] != '\\' ||
                text_[position_ + 1] != 'u')
              return fail("unpaired high surrogate");
            position_ += 2;
            uint32_t low = 0;
            if (!parseHex4(low)) return false;
            if (low < 0xdc00 || low > 0xdfff) return fail("invalid low surrogate");
            cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
          } else if (cp >= 0xdc00 && cp <= 0xdfff) {
            return fail("unpaired low surrogate");
          }
          appendUtf8(out, cp);
          break;
        }
        default:
          return fail("invalid escape character");
      }
    }
    return fail("unterminated string");
  }

  bool parseNumber(JsonValue& out) {
    const size_t start = position_;
    if (position_ < text_.size() && text_[position_] == '-') ++position_;
    if (position_ >= text_.size()) return fail("truncated number");
    if (text_[position_] == '0') {
      ++position_;
    } else if (text_[position_] >= '1' && text_[position_] <= '9') {
      while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9')
        ++position_;
    } else {
      return fail("invalid number");
    }
    bool floating = false;
    if (position_ < text_.size() && text_[position_] == '.') {
      floating = true;
      ++position_;
      if (position_ >= text_.size() || text_[position_] < '0' || text_[position_] > '9')
        return fail("invalid fraction");
      while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9')
        ++position_;
    }
    if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
      floating = true;
      ++position_;
      if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-'))
        ++position_;
      if (position_ >= text_.size() || text_[position_] < '0' || text_[position_] > '9')
        return fail("invalid exponent");
      while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9')
        ++position_;
    }
    const std::string token(text_.substr(start, position_ - start));
    if (!floating) {
      errno = 0;
      char* end = nullptr;
      const long long value = std::strtoll(token.c_str(), &end, 10);
      if (errno == 0 && end == token.c_str() + token.size()) {
        out = JsonValue::makeInt(static_cast<int64_t>(value));
        return true;
      }
    }
    const double value = std::strtod(token.c_str(), nullptr);
    if (!std::isfinite(value)) return fail("number out of range");
    out = JsonValue::makeDouble(value);
    return true;
  }

  std::string_view text_;
  size_t max_depth_;
  size_t position_ = 0;
  JsonParseError error_;
};

}  // namespace

JsonValue JsonValue::makeNull() { return JsonValue(); }
JsonValue JsonValue::makeBool(bool value) {
  JsonValue json;
  json.type_ = Type::Bool;
  json.bool_ = value;
  return json;
}
JsonValue JsonValue::makeInt(int64_t value) {
  JsonValue json;
  json.type_ = Type::Int;
  json.int_ = value;
  return json;
}
JsonValue JsonValue::makeDouble(double value) {
  JsonValue json;
  json.type_ = Type::Double;
  json.double_ = value;
  return json;
}
JsonValue JsonValue::makeString(std::string value) {
  JsonValue json;
  json.type_ = Type::String;
  json.string_ = std::move(value);
  return json;
}
JsonValue JsonValue::makeArray(Array value) {
  JsonValue json;
  json.type_ = Type::Array;
  json.array_ = std::move(value);
  return json;
}
JsonValue JsonValue::makeObject(Object value) {
  JsonValue json;
  json.type_ = Type::Object;
  json.object_ = std::move(value);
  return json;
}

bool JsonValue::boolValue(bool fallback) const {
  if (type_ == Type::Bool) return bool_;
  if (type_ == Type::Int) return int_ != 0;
  return fallback;
}
int64_t JsonValue::intValue(int64_t fallback) const {
  if (type_ == Type::Int) return int_;
  if (type_ == Type::Double && std::isfinite(double_) && double_ < 9223372036854775808.0 &&
      double_ >= -9223372036854775808.0)
    return static_cast<int64_t>(double_);
  if (type_ == Type::Bool) return bool_ ? 1 : 0;
  return fallback;
}
double JsonValue::doubleValue(double fallback) const {
  if (type_ == Type::Double) return double_;
  if (type_ == Type::Int) return static_cast<double>(int_);
  if (type_ == Type::Bool) return bool_ ? 1.0 : 0.0;
  return fallback;
}
const std::string& JsonValue::stringValue() const {
  return type_ == Type::String ? string_ : kEmptyString;
}
const JsonValue::Array& JsonValue::arrayItems() const {
  return type_ == Type::Array ? array_ : kEmptyArray;
}
JsonValue::Array& JsonValue::arrayItems() { return array_; }
const JsonValue::Object& JsonValue::objectItems() const {
  return type_ == Type::Object ? object_ : kEmptyObject;
}
JsonValue::Object& JsonValue::objectItems() { return object_; }
const JsonValue* JsonValue::find(std::string_view key) const {
  if (type_ != Type::Object) return nullptr;
  for (const auto& member : object_) {
    if (member.first == key) return &member.second;
  }
  return nullptr;
}
JsonValue* JsonValue::find(std::string_view key) {
  if (type_ != Type::Object) return nullptr;
  for (auto& member : object_) {
    if (member.first == key) return &member.second;
  }
  return nullptr;
}
std::string JsonValue::toString() const {
  std::string out;
  writeValue(out, *this);
  return out;
}
std::string JsonValue::scalarText() const {
  switch (type_) {
    case Type::Null: return "null";
    case Type::Bool: return bool_ ? "true" : "false";
    case Type::Int: return std::to_string(int_);
    case Type::Double: {
      char buffer[40];
      if (!std::isfinite(double_)) return "null";
      std::snprintf(buffer, sizeof(buffer), "%.17g", double_);
      return buffer;
    }
    case Type::String: return string_;
    default: return {};
  }
}

bool jsonParse(std::string_view text, JsonValue& out, JsonParseError& err, size_t max_depth) {
  Parser parser(text, max_depth);
  if (parser.parse(out)) return true;
  err = parser.error();
  return false;
}

bool jsonIsValidUtf8(std::string_view text) {
  size_t position = 0;
  while (position < text.size()) {
    const unsigned char first = static_cast<unsigned char>(text[position]);
    if (first <= 0x7f) {
      ++position;
      continue;
    }
    size_t length = 0;
    uint32_t cp = 0;
    uint32_t minimum = 0;
    if (first >= 0xc2 && first <= 0xdf) {
      length = 2;
      cp = first & 0x1f;
      minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
      length = 3;
      cp = first & 0x0f;
      minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
      length = 4;
      cp = first & 0x07;
      minimum = 0x10000;
    } else {
      return false;
    }
    if (length > text.size() - position) return false;
    ++position;
    for (size_t i = 1; i < length; ++i, ++position) {
      const unsigned char next = static_cast<unsigned char>(text[position]);
      if ((next & 0xc0) != 0x80) return false;
      cp = (cp << 6) | (next & 0x3f);
    }
    if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
  }
  return true;
}

size_t jsonCodePointCount(std::string_view text) {
  if (!jsonIsValidUtf8(text)) return std::string_view::npos;
  size_t count = 0;
  for (const unsigned char c : text) {
    if ((c & 0xc0) != 0x80) ++count;
  }
  return count;
}

std::string jsonEscape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  for (const unsigned char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
          out += buffer;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  return out;
}

}  // namespace eego::json
