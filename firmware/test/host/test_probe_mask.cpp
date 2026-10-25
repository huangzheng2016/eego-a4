// SPDX-License-Identifier: GPL-3.0-or-later
// Source probe text masking (admin/src/probe_mask.cpp).
#include <string>

#include "probe_mask.h"
#include "test_framework.h"

using eego::admin::internal::probeMaskText;

namespace {

std::string mask(const std::string& in, size_t* count = nullptr) {
  size_t masked = 0;
  std::string out = probeMaskText(in, masked);
  if (count != nullptr) *count = masked;
  return out;
}

}  // namespace

TEST(probe_mask, header_value_masked_to_end_of_line) {
  size_t n = 0;
  CHECK_EQ(mask("Authorization: Bearer abc.def\r\nHost: x\r\n", &n),
           std::string("Authorization: ********\r\nHost: x\r\n"));
  CHECK_EQ(n, size_t{1});
  CHECK_EQ(mask("Cookie: a=1; b=2"), std::string("Cookie: ********"));
  CHECK_EQ(mask("X-Api-Key: k1 k2\nnext: line"), std::string("X-Api-Key: ********\nnext: line"));
  CHECK_EQ(mask("password:\nuser: bob"), std::string("password:\nuser: bob"));
}

TEST(probe_mask, key_value_stops_at_separators) {
  CHECK_EQ(mask("a=1&token=abc&b=2"), std::string("a=1&token=********&b=2"));
  CHECK_EQ(mask("api_key=xyz;path=/"), std::string("api_key=********;path=/"));
  CHECK_EQ(mask("secret=s1,other=2"), std::string("secret=********,other=2"));
  CHECK_EQ(mask("pass=p w"), std::string("pass=******** w"));
}

TEST(probe_mask, json_ish_bodies) {
  size_t n = 0;
  CHECK_EQ(mask(R"({"token":"abc","n":1,"apiKey": 1234, "auth": {"u":"x"}})", &n),
           std::string(R"({"token":"********","n":1,"apiKey": ********, "auth": "********"})"));
  CHECK_EQ(n, size_t{3});
  CHECK_EQ(mask(R"({"password":"a\"b"})"), std::string(R"({"password":"********"})"));
  CHECK_EQ(mask(R"({"name":"keyboard"})"), std::string(R"({"name":"keyboard"})"));
}

TEST(probe_mask, non_sensitive_text_unchanged) {
  const std::string text = "temperature=21.5&humidity=40\nStatus: ok\n";
  size_t n = 0;
  CHECK_EQ(mask(text, &n), text);
  CHECK_EQ(n, size_t{0});
}
