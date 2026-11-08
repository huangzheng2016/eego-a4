// SPDX-License-Identifier: GPL-3.0-or-later
// Shared JSON DOM parser (json_dom): results after the in-place parsing rewrite.
#include "json_dom/json.h"
#include "test_framework.h"

using eego::json::JsonParseError;
using eego::json::JsonValue;
using eego::json::jsonParse;

TEST(json_parser, round_trips_nested_documents) {
  const char* text =
      R"({"a":[1,2.5,-3e2,true,false,null,"xé\n"],"b":{"c":{"d":[[],{}]}},"e":"😀"})";
  JsonValue v;
  JsonParseError err;
  REQUIRE(jsonParse(text, v, err));
  JsonValue again;
  REQUIRE(jsonParse(v.toString(), again, err));
  CHECK_EQ(v.toString(), again.toString());
  CHECK_EQ(v.find("a")->arrayItems().size(), static_cast<size_t>(7));
  CHECK(v.find("a")->arrayItems()[0].type() == JsonValue::Type::Int);
  CHECK(v.find("a")->arrayItems()[1].type() == JsonValue::Type::Double);
  CHECK(v.find("b")->find("c")->find("d")->arrayItems()[1].type() == JsonValue::Type::Object);
  CHECK_EQ(v.objectItems()[0].first, std::string("a"));  // member order preserved
}

TEST(json_parser, failure_leaves_output_untouched) {
  JsonValue v = JsonValue::makeInt(7);
  JsonParseError err;
  CHECK(!jsonParse(R"({"a":[1,2,{"b":}]})", v, err));
  CHECK(v.type() == JsonValue::Type::Int);
  CHECK(!jsonParse(R"({"a":1,"a":2})", v, err));
  CHECK_EQ(err.message, std::string("duplicate object key"));
  CHECK(!jsonParse(R"({"a" 1})", v, err));
  CHECK_EQ(err.message, std::string("expected ':' after key"));
  CHECK(!jsonParse(R"([1 2])", v, err));
  CHECK_EQ(err.message, std::string("expected ',' or ']' in array"));
}

TEST(json_parser, depth_limit) {
  // The root is depth 0, so max_depth 16 allows 17 nested levels.
  std::string ok(17, '[');
  ok += std::string(17, ']');
  JsonValue v;
  JsonParseError err;
  CHECK(jsonParse(ok, v, err, 16));
  std::string deep(18, '[');
  deep += std::string(18, ']');
  CHECK(!jsonParse(deep, v, err, 16));
  CHECK_EQ(err.message, std::string("JSON nesting exceeds configured depth"));
  std::string huge(5000, '[');
  CHECK(!jsonParse(huge, v, err));
}
