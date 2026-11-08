// SPDX-License-Identifier: GPL-3.0-or-later
// Proxy request/response heads for the BLE uplink (pure, host-tested).
#include <algorithm>
#include <cctype>
#include <cstdio>

#include "alert_core/json.h"
#include "net_stack/uplink.h"

namespace eego::net::uplink {
namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

void member(std::string& out, const char* key, const std::string& value) {
  out += '"';
  out += key;
  out += "\":\"";
  out += alert_core::jsonEscape(value);
  out += '"';
}

}  // namespace

const char* modeName(Mode mode) {
  switch (mode) {
    case Mode::Wifi: return "wifi";
    case Mode::Ble: return "ble";
    default: return "auto";
  }
}

std::string buildRequestHead(const NetRequest& req, uint32_t max_body,
                             const std::string& if_none_match, const std::string& if_modified_since) {
  std::map<std::string, std::string> headers;
  headers["user-agent"] = "eego-a4";
  headers["accept"] = "application/json, */*";
  if (req.method == "POST") headers["content-type"] = "application/json";
  for (const auto& h : req.headers) headers[lower(h.name)] = h.value;
  if (!if_none_match.empty()) headers["if-none-match"] = if_none_match;
  if (!if_modified_since.empty()) headers["if-modified-since"] = if_modified_since;

  std::string out = "{";
  member(out, "m", req.method.empty() ? std::string("GET") : req.method);
  out += ',';
  member(out, "u", req.url);
  out += ",\"h\":{";
  bool first = true;
  for (const auto& kv : headers) {
    if (!first) out += ',';
    first = false;
    out += '"';
    out += alert_core::jsonEscape(kv.first);
    out += "\":\"";
    out += alert_core::jsonEscape(kv.second);
    out += '"';
  }
  char tail[64];
  std::snprintf(tail, sizeof(tail), "},\"t\":%lu,\"max\":%lu}",
                static_cast<unsigned long>(req.timeout_ms), static_cast<unsigned long>(max_body));
  out += tail;
  return out;
}

bool parseResponseHead(const std::string& head, int& status,
                       std::map<std::string, std::string>& headers, std::string& error) {
  status = 0;
  headers.clear();
  error.clear();
  alert_core::JsonValue root;
  alert_core::JsonParseError perr;
  if (!alert_core::jsonParse(head, root, perr, 3) ||
      root.type() != alert_core::JsonValue::Type::Object) {
    error = "agent: malformed response";
    return false;
  }
  const alert_core::JsonValue* s = root.find("s");
  if (s == nullptr || s->type() != alert_core::JsonValue::Type::Int) {
    error = "agent: malformed response";
    return false;
  }
  status = static_cast<int>(s->intValue());
  if (status == 0) {
    const alert_core::JsonValue* e = root.find("e");
    const alert_core::JsonValue* m = root.find("m");
    error = "agent: ";
    error += (e != nullptr && e->type() == alert_core::JsonValue::Type::String) ? e->stringValue()
                                                                                 : std::string("network");
    if (m != nullptr && m->type() == alert_core::JsonValue::Type::String && !m->stringValue().empty()) {
      error += ": " + m->stringValue().substr(0, 96);
    }
    return true;
  }
  if (const alert_core::JsonValue* h = root.find("h");
      h != nullptr && h->type() == alert_core::JsonValue::Type::Object) {
    for (const auto& kv : h->objectItems()) {
      if (kv.second.type() == alert_core::JsonValue::Type::String) {
        headers[lower(kv.first)] = kv.second.stringValue();
      }
    }
  }
  return true;
}

}  // namespace eego::net::uplink
