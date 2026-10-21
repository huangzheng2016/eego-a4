// SPDX-License-Identifier: GPL-3.0-or-later
#include "data_pipeline/json_path.h"

#include <cstdint>

namespace eego::dashboard::pipeline {
namespace {

enum class TokenKind : uint8_t { Child, Index, Wildcard };

struct Token {
  TokenKind kind = TokenKind::Child;
  std::string key;
  size_t index = 0;
};

bool parsePath(std::string_view path, std::vector<Token>& tokens, std::string& error) {
  tokens.clear();
  if (path.empty() || path.size() > 1024) {
    error = "JSONPath is empty or too long";
    return false;
  }
  size_t position = 0;
  if (path.front() == '$') {
    position = 1;
  } else if (path.front() == '.' || path.front() == '[') {
    error = "JSONPath must start with $ or a child name";
    return false;
  }

  while (position < path.size()) {
    if (tokens.size() >= 64) {
      error = "JSONPath exceeds segment limit";
      return false;
    }
    if (path[position] == '.') {
      ++position;
      if (position >= path.size() || path[position] == '.' || path[position] == '[') {
        error = "invalid dot segment in JSONPath";
        return false;
      }
      if (path[position] == '*') {
        ++position;
        if (position < path.size() && path[position] != '.' && path[position] != '[') {
          error = "invalid wildcard in JSONPath";
          return false;
        }
        tokens.push_back({TokenKind::Wildcard, {}, 0});
        continue;
      }
      const size_t start = position;
      while (position < path.size() && path[position] != '.' && path[position] != '[') {
        ++position;
      }
      if (position == start) {
        error = "empty child in JSONPath";
        return false;
      }
      tokens.push_back({TokenKind::Child, std::string(path.substr(start, position - start)), 0});
      continue;
    }

    if (path[position] == '[') {
      ++position;
      if (position >= path.size()) {
        error = "unterminated bracket in JSONPath";
        return false;
      }
      if (path[position] == '*') {
        ++position;
        if (position >= path.size() || path[position++] != ']') {
          error = "wildcard bracket must be [*]";
          return false;
        }
        tokens.push_back({TokenKind::Wildcard, {}, 0});
        continue;
      }
      if (path[position] == '\'' || path[position] == '"') {
        const char quote = path[position++];
        std::string key;
        bool closed = false;
        while (position < path.size()) {
          const char c = path[position++];
          if (c == quote) {
            closed = true;
            break;
          }
          if (c == '\\') {
            if (position >= path.size()) {
              error = "truncated escape in JSONPath";
              return false;
            }
            const char escaped = path[position++];
            if (escaped != quote && escaped != '\\') {
              error = "unsupported escape in JSONPath";
              return false;
            }
            key.push_back(escaped);
          } else {
            if (static_cast<unsigned char>(c) < 0x20) {
              error = "control character in JSONPath";
              return false;
            }
            key.push_back(c);
          }
          if (key.size() > 256) {
            error = "JSONPath key is too long";
            return false;
          }
        }
        if (!closed || key.empty() || position >= path.size() || path[position++] != ']') {
          error = "invalid quoted child in JSONPath";
          return false;
        }
        tokens.push_back({TokenKind::Child, std::move(key), 0});
        continue;
      }

      size_t index = 0;
      size_t digits = 0;
      while (position < path.size() && path[position] >= '0' && path[position] <= '9') {
        if (digits++ >= 9) {
          error = "JSONPath array index is too large";
          return false;
        }
        index = index * 10 + static_cast<size_t>(path[position++] - '0');
      }
      if (digits == 0 || position >= path.size() || path[position++] != ']') {
        error = "invalid array index in JSONPath";
        return false;
      }
      tokens.push_back({TokenKind::Index, {}, index});
      continue;
    }

    if (!tokens.empty() || position != 0) {
      error = "expected dot or bracket segment in JSONPath";
      return false;
    }
    const size_t start = position;
    while (position < path.size() && path[position] != '.' && path[position] != '[') ++position;
    if (position == start) {
      error = "empty child in JSONPath";
      return false;
    }
    tokens.push_back({TokenKind::Child, std::string(path.substr(start, position - start)), 0});
  }
  return true;
}

}  // namespace

bool validateJsonPath(std::string_view path, std::string& error) {
  std::vector<Token> tokens;
  return parsePath(path, tokens, error);
}

bool selectJsonPath(const JsonValue& root, std::string_view path,
                    std::vector<const JsonValue*>& out, std::string& error, size_t max_results) {
  out.clear();
  std::vector<Token> tokens;
  if (!parsePath(path, tokens, error)) return false;
  out.push_back(&root);
  for (const Token& token : tokens) {
    std::vector<const JsonValue*> next;
    for (const JsonValue* current : out) {
      if (token.kind == TokenKind::Child) {
        if (current->type() != JsonValue::Type::Object) continue;
        if (const JsonValue* child = current->find(token.key)) next.push_back(child);
      } else if (token.kind == TokenKind::Index) {
        if (current->type() != JsonValue::Type::Array) continue;
        const auto& items = current->arrayItems();
        if (token.index < items.size()) next.push_back(&items[token.index]);
      } else if (current->type() == JsonValue::Type::Array) {
        for (const JsonValue& item : current->arrayItems()) next.push_back(&item);
      } else if (current->type() == JsonValue::Type::Object) {
        for (const auto& member : current->objectItems()) next.push_back(&member.second);
      }
      if (next.size() > max_results) {
        out.clear();
        error = "JSONPath result exceeds configured limit";
        return false;
      }
    }
    out = std::move(next);
    if (out.empty()) break;
  }
  return true;
}

const JsonValue* jsonPathFirst(const JsonValue& root, std::string_view path) {
  std::vector<const JsonValue*> selected;
  std::string error;
  if (!selectJsonPath(root, path, selected, error) || selected.empty()) return nullptr;
  return selected.front();
}

}  // namespace eego::dashboard::pipeline
