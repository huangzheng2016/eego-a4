// SPDX-License-Identifier: GPL-3.0-or-later
#include <map>
#include <string>

#include "json_dom/json.h"
#include "net_stack/uplink.h"
#include "test_framework.h"

using namespace eego::net;

TEST(uplink, request_head_carries_method_url_headers_and_limits) {
  NetRequest req;
  req.method = "POST";
  req.url = "https://api.example.com/v1/metrics?x=\"1\"";
  req.timeout_ms = 15000;
  req.headers.push_back({"Authorization", "Bearer secret"});
  req.headers.push_back({"X-Mode", "a\"b"});
  const std::string head = uplink::buildRequestHead(req, 4096, "\"v1\"", "");

  eego::json::JsonValue root;
  eego::json::JsonParseError err;
  REQUIRE(eego::json::jsonParse(head, root, err, 3));
  CHECK_EQ(root.find("m")->stringValue(), std::string("POST"));
  CHECK_EQ(root.find("u")->stringValue(), req.url);
  CHECK_EQ(root.find("t")->intValue(), 15000);
  CHECK_EQ(root.find("max")->intValue(), 4096);
  const eego::json::JsonValue* h = root.find("h");
  REQUIRE(h != nullptr);
  CHECK_EQ(h->find("authorization")->stringValue(), std::string("Bearer secret"));
  CHECK_EQ(h->find("x-mode")->stringValue(), std::string("a\"b"));
  CHECK_EQ(h->find("content-type")->stringValue(), std::string("application/json"));
  CHECK_EQ(h->find("if-none-match")->stringValue(), std::string("\"v1\""));
  CHECK(h->find("if-modified-since") == nullptr);
  CHECK(h->find("user-agent") != nullptr);
}

TEST(uplink, response_head_success_and_failure) {
  int status = -1;
  std::map<std::string, std::string> headers;
  std::string error;
  REQUIRE(uplink::parseResponseHead(R"({"s":200,"h":{"Content-Type":"text/plain","etag":"\"x\""}})",
                                    status, headers, error));
  CHECK_EQ(status, 200);
  CHECK_EQ(headers["content-type"], std::string("text/plain"));
  CHECK_EQ(headers["etag"], std::string("\"x\""));
  CHECK(error.empty());

  REQUIRE(uplink::parseResponseHead(R"({"s":0,"e":"dns","m":"no such host"})", status, headers, error));
  CHECK_EQ(status, 0);
  CHECK_EQ(error, std::string("agent: dns: no such host"));

  CHECK(!uplink::parseResponseHead("not json", status, headers, error));
  CHECK(!uplink::parseResponseHead(R"({"h":{}})", status, headers, error));
}

TEST(uplink, mode_names) {
  CHECK_EQ(std::string(uplink::modeName(uplink::Mode::Auto)), std::string("auto"));
  CHECK_EQ(std::string(uplink::modeName(uplink::Mode::Wifi)), std::string("wifi"));
  CHECK_EQ(std::string(uplink::modeName(uplink::Mode::Ble)), std::string("ble"));
}
