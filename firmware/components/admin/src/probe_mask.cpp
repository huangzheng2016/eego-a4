// SPDX-License-Identifier: GPL-3.0-or-later
#include "probe_mask.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace eego::admin::internal {
namespace {

bool keyChar(unsigned char c) {
  return std::isalnum(c) != 0 || c == '_' || c == '-' || c == '.';
}

bool isQuote(char c) { return c == '"' || c == '\''; }

// Index just past a quoted string starting at `open` (an opening quote). Stops
// at the end of the line when the quote is never closed.
size_t skipQuoted(const std::string& input, size_t open, bool& closed) {
  const char quote = input[open];
  size_t close = open + 1;
  while (close < input.size() && input[close] != quote && input[close] != '\n') {
    if (input[close] == '\\') ++close;
    ++close;
  }
  closed = close < input.size() && input[close] == quote;
  if (closed) ++close;
  return std::min(close, input.size());
}

// Index just past a JSON object/array starting at `open`.
size_t skipNested(const std::string& input, size_t open) {
  size_t close = open;
  int depth = 0;
  bool in_string = false;
  for (; close < input.size(); ++close) {
    const char c = input[close];
    if (in_string) {
      if (c == '\\') ++close;
      else if (c == '"') in_string = false;
    } else if (c == '"') {
      in_string = true;
    } else if (c == '{' || c == '[') {
      ++depth;
    } else if ((c == '}' || c == ']') && --depth == 0) {
      ++close;
      break;
    }
  }
  return std::min(close, input.size());
}

size_t skipUntil(const std::string& input, size_t from, const char* stops) {
  size_t stop = from;
  while (stop < input.size() && std::strchr(stops, input[stop]) == nullptr) ++stop;
  return stop;
}

}  // namespace

bool previewSensitive(const std::string& key) {
  std::string lowered = key;
  for (char& c : lowered) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return lowered.find("token") != std::string::npos || lowered.find("secret") != std::string::npos ||
         lowered.find("pass") != std::string::npos || lowered.find("key") != std::string::npos ||
         lowered.find("auth") != std::string::npos || lowered.find("cookie") != std::string::npos;
}

std::string probeMaskText(const std::string& input, size_t& masked) {
  std::string out;
  out.reserve(input.size());
  size_t i = 0;
  while (i < input.size()) {
    if (!keyChar(static_cast<unsigned char>(input[i]))) {
      out.push_back(input[i++]);
      continue;
    }
    const size_t key_start = i;
    size_t key_end = i;
    while (key_end < input.size() && keyChar(static_cast<unsigned char>(input[key_end]))) ++key_end;
    out.append(input, i, key_end - i);
    const bool sensitive = previewSensitive(input.substr(i, key_end - i));
    i = key_end;
    if (!sensitive) continue;
    const bool quoted_key = key_start > 0 && isQuote(input[key_start - 1]);
    size_t cursor = i;
    if (quoted_key && cursor < input.size() && input[cursor] == input[key_start - 1]) ++cursor;
    while (cursor < input.size() && (input[cursor] == ' ' || input[cursor] == '\t')) ++cursor;
    if (cursor >= input.size() || (input[cursor] != ':' && input[cursor] != '=')) continue;
    const char separator = input[cursor];
    ++cursor;
    while (cursor < input.size() && (input[cursor] == ' ' || input[cursor] == '\t')) ++cursor;
    out.append(input, i, cursor - i);
    i = cursor;
    if (i >= input.size() || input[i] == '\r' || input[i] == '\n') {
      // "Key:" with nothing after it on the line: nothing to hide.
      continue;
    }

    if (!quoted_key && separator == ':') {
      // Header / YAML style ("Authorization: Bearer abc", "Cookie: a=1; b=2"):
      // the value runs to the end of the line, spaces and separators included.
      i = skipUntil(input, i, "\r\n");
      out += kProbeMask;
    } else if (isQuote(input[i])) {
      bool closed = false;
      const char quote = input[i];
      i = skipQuoted(input, i, closed);
      out.push_back(quote);
      out += kProbeMask;
      if (closed) out.push_back(quote);
    } else if (quoted_key && (input[i] == '{' || input[i] == '[')) {
      // Nested structure under a sensitive JSON key: mask it entirely.
      i = skipNested(input, i);
      out += '"';
      out += kProbeMask;
      out += '"';
    } else if (quoted_key) {
      // JSON scalar (number, literal): ends at the next JSON delimiter.
      i = skipUntil(input, i, ",}] \t\r\n");
      out += kProbeMask;
    } else {
      // key=value (query string, form body, cookie pair).
      i = skipUntil(input, i, ",&; \t\r\n\"'");
      out += kProbeMask;
    }
    ++masked;
  }
  return out;
}

}  // namespace eego::admin::internal
