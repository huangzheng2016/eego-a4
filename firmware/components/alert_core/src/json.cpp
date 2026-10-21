#include "alert_core/json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace alert_core {
namespace {

const JsonValue::Array kEmptyArray;
const JsonValue::Object kEmptyObject;
const std::string kEmptyString;

void writeJsonString(std::string& out, std::string_view s) {
  out.push_back('"');
  out += jsonEscape(s);
  out.push_back('"');
}

void writeJsonValue(std::string& out, const JsonValue& v) {
  switch (v.type()) {
    case JsonValue::Type::Null:
      out += "null";
      break;
    case JsonValue::Type::Bool:
      out += v.boolValue() ? "true" : "false";
      break;
    case JsonValue::Type::Int:
      out += std::to_string(v.intValue());
      break;
    case JsonValue::Type::Double: {
      double d = v.doubleValue();
      if (d == static_cast<double>(static_cast<int64_t>(d)) && std::fabs(d) < 1e15) {
        out += std::to_string(static_cast<int64_t>(d));
      } else {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.17g", d);
        out += buf;
      }
      break;
    }
    case JsonValue::Type::String:
      writeJsonString(out, v.stringValue());
      break;
    case JsonValue::Type::Array: {
      out.push_back('[');
      bool first = true;
      for (const JsonValue& item : v.arrayItems()) {
        if (!first) out.push_back(',');
        first = false;
        writeJsonValue(out, item);
      }
      out.push_back(']');
      break;
    }
    case JsonValue::Type::Object: {
      out.push_back('{');
      bool first = true;
      for (const auto& kv : v.objectItems()) {
        if (!first) out.push_back(',');
        first = false;
        writeJsonString(out, kv.first);
        out.push_back(':');
        writeJsonValue(out, kv.second);
      }
      out.push_back('}');
      break;
    }
  }
}

// Appends UTF-8 encoding of code point `cp` to `out`.
void appendUtf8(std::string& out, uint32_t cp) {
  if (cp <= 0x7F) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

class Parser {
 public:
  Parser(const std::string& text) : text_(text) {}

  bool parseDocument(JsonValue& out) {
    skipWs();
    if (!parseValue(out, 0)) return false;
    skipWs();
    if (pos_ != text_.size()) return fail("trailing garbage after JSON value");
    return true;
  }

  const JsonParseError& error() const { return err_; }

 private:
  static constexpr int kMaxDepth = 16;

  bool fail(const char* msg) {
    err_.offset = pos_;
    err_.message = msg;
    return false;
  }

  void skipWs() {
    while (pos_ < text_.size()) {
      char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  bool literal(std::string_view rest) {
    if (text_.compare(pos_, rest.size(), rest) != 0) return fail("invalid literal");
    pos_ += rest.size();
    return true;
  }

  bool parseValue(JsonValue& out, int depth) {
    if (depth > kMaxDepth) return fail("nesting too deep");
    if (pos_ >= text_.size()) return fail("unexpected end of input");
    switch (text_[pos_]) {
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
        std::string s;
        if (!parseString(s)) return false;
        out = JsonValue::makeString(std::move(s));
        return true;
      }
      case '[': {
        ++pos_;
        JsonValue::Array items;
        skipWs();
        if (pos_ < text_.size() && text_[pos_] == ']') {
          ++pos_;
          out = JsonValue::makeArray(std::move(items));
          return true;
        }
        for (;;) {
          JsonValue item;
          skipWs();
          if (!parseValue(item, depth + 1)) return false;
          items.push_back(std::move(item));
          skipWs();
          if (pos_ >= text_.size()) return fail("unterminated array");
          if (text_[pos_] == ',') {
            ++pos_;
            continue;
          }
          if (text_[pos_] == ']') {
            ++pos_;
            out = JsonValue::makeArray(std::move(items));
            return true;
          }
          return fail("expected ',' or ']' in array");
        }
      }
      case '{': {
        ++pos_;
        JsonValue::Object members;
        skipWs();
        if (pos_ < text_.size() && text_[pos_] == '}') {
          ++pos_;
          out = JsonValue::makeObject(std::move(members));
          return true;
        }
        for (;;) {
          skipWs();
          if (pos_ >= text_.size() || text_[pos_] != '"') return fail("expected object key");
          std::string key;
          if (!parseString(key)) return false;
          skipWs();
          if (pos_ >= text_.size() || text_[pos_] != ':') return fail("expected ':' after key");
          ++pos_;
          skipWs();
          JsonValue value;
          if (!parseValue(value, depth + 1)) return false;
          members.emplace_back(std::move(key), std::move(value));
          skipWs();
          if (pos_ >= text_.size()) return fail("unterminated object");
          if (text_[pos_] == ',') {
            ++pos_;
            continue;
          }
          if (text_[pos_] == '}') {
            ++pos_;
            out = JsonValue::makeObject(std::move(members));
            return true;
          }
          return fail("expected ',' or '}' in object");
        }
      }
      default:
        return parseNumber(out);
    }
  }

  bool parseHex4(uint32_t& cp) {
    if (pos_ + 4 > text_.size()) return fail("truncated \\u escape");
    cp = 0;
    for (int i = 0; i < 4; ++i) {
      char c = text_[pos_ + i];
      cp <<= 4;
      if (c >= '0' && c <= '9') {
        cp |= static_cast<uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        cp |= static_cast<uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        cp |= static_cast<uint32_t>(c - 'A' + 10);
      } else {
        return fail("invalid hex digit in \\u escape");
      }
    }
    pos_ += 4;
    return true;
  }

  bool parseString(std::string& out) {
    ++pos_;  // opening quote
    while (pos_ < text_.size()) {
      unsigned char c = static_cast<unsigned char>(text_[pos_]);
      if (c == '"') {
        ++pos_;
        return true;
      }
      if (c < 0x20) return fail("unescaped control character in string");
      if (c != '\\') {
        out.push_back(static_cast<char>(c));
        ++pos_;
        continue;
      }
      ++pos_;
      if (pos_ >= text_.size()) return fail("truncated escape");
      char e = text_[pos_++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          uint32_t cp;
          if (!parseHex4(cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            // High surrogate: a low surrogate must follow.
            if (pos_ + 1 < text_.size() && text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
              pos_ += 2;
              uint32_t lo;
              if (!parseHex4(lo)) return false;
              if (lo < 0xDC00 || lo > 0xDFFF) return fail("invalid low surrogate");
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else {
              return fail("unpaired high surrogate");
            }
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
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
    size_t start = pos_;
    if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
    if (pos_ >= text_.size()) return fail("truncated number");
    if (text_[pos_] == '0') {
      ++pos_;
    } else if (text_[pos_] >= '1' && text_[pos_] <= '9') {
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
    } else {
      return fail("invalid number");
    }
    bool is_double = false;
    if (pos_ < text_.size() && text_[pos_] == '.') {
      is_double = true;
      ++pos_;
      if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9')
        return fail("invalid fraction");
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      is_double = true;
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
      if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9')
        return fail("invalid exponent");
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
    }
    std::string token = text_.substr(start, pos_ - start);
    if (!is_double) {
      errno = 0;
      char* end = nullptr;
      long long v = std::strtoll(token.c_str(), &end, 10);
      if (errno == 0 && end == token.c_str() + token.size()) {
        out = JsonValue::makeInt(static_cast<int64_t>(v));
        return true;
      }
      // Fall through to double on overflow.
    }
    out = JsonValue::makeDouble(std::strtod(token.c_str(), nullptr));
    return true;
  }

  const std::string& text_;
  size_t pos_ = 0;
  JsonParseError err_;
};

}  // namespace

JsonValue JsonValue::makeNull() { return JsonValue(); }

JsonValue JsonValue::makeBool(bool v) {
  JsonValue j;
  j.type_ = Type::Bool;
  j.bool_ = v;
  return j;
}

JsonValue JsonValue::makeInt(int64_t v) {
  JsonValue j;
  j.type_ = Type::Int;
  j.int_ = v;
  return j;
}

JsonValue JsonValue::makeDouble(double v) {
  JsonValue j;
  j.type_ = Type::Double;
  j.double_ = v;
  return j;
}

JsonValue JsonValue::makeString(std::string v) {
  JsonValue j;
  j.type_ = Type::String;
  j.string_ = std::move(v);
  return j;
}

JsonValue JsonValue::makeArray(Array v) {
  JsonValue j;
  j.type_ = Type::Array;
  j.array_ = std::move(v);
  return j;
}

JsonValue JsonValue::makeObject(Object v) {
  JsonValue j;
  j.type_ = Type::Object;
  j.object_ = std::move(v);
  return j;
}

bool JsonValue::boolValue(bool fallback) const {
  if (type_ == Type::Bool) return bool_;
  if (type_ == Type::Int) return int_ != 0;
  return fallback;
}

int64_t JsonValue::intValue(int64_t fallback) const {
  if (type_ == Type::Int) return int_;
  if (type_ == Type::Double) return static_cast<int64_t>(double_);
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

const JsonValue::Object& JsonValue::objectItems() const {
  return type_ == Type::Object ? object_ : kEmptyObject;
}

const JsonValue* JsonValue::find(std::string_view key) const {
  if (type_ != Type::Object) return nullptr;
  for (const auto& kv : object_) {
    if (kv.first == key) return &kv.second;
  }
  return nullptr;
}

std::string JsonValue::toString() const {
  std::string out;
  writeJsonValue(out, *this);
  return out;
}

std::string JsonValue::scalarText() const {
  switch (type_) {
    case Type::Null:
      return "null";
    case Type::Bool:
      return bool_ ? "true" : "false";
    case Type::Int:
      return std::to_string(int_);
    case Type::Double: {
      double d = double_;
      if (d == static_cast<double>(static_cast<int64_t>(d)) && std::fabs(d) < 1e15) {
        return std::to_string(static_cast<int64_t>(d));
      }
      char buf[40];
      std::snprintf(buf, sizeof(buf), "%.17g", d);
      return buf;
    }
    case Type::String:
      return string_;
    default:
      return std::string();
  }
}

bool jsonParse(const std::string& text, JsonValue& out, JsonParseError& err) {
  Parser p(text);
  if (p.parseDocument(out)) return true;
  err = p.error();
  return false;
}

std::string jsonEscape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  for (unsigned char c : s) {
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
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  return out;
}

}  // namespace alert_core
