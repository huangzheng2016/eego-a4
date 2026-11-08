// SPDX-License-Identifier: GPL-3.0-or-later
// BLE agent uplink routing (see include/net_stack/uplink.h).
#include "net_stack/uplink.h"

#include <atomic>
#include <mutex>

#include "esp_heap_caps.h"
#include "esp_log.h"

namespace eego::net::uplink {
namespace {

constexpr char TAG[] = "uplink";
constexpr size_t kMaxValidators = 32;

std::atomic<Mode> g_mode{Mode::Auto};
Agent g_agent;
std::mutex g_mutex;

struct Validator {
  std::string etag;
  std::string last_modified;
};
std::map<std::string, Validator> g_validators;  // by URL, for conditional GET over the agent

std::string hostOf(const std::string& url) {
  const size_t scheme = url.find("://");
  const size_t start = scheme == std::string::npos ? 0 : scheme + 3;
  const size_t end = url.find_first_of("/?#", start);
  std::string host = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
  const size_t at = host.rfind('@');  // never log userinfo
  return at == std::string::npos ? host : host.substr(at + 1);
}

const char* fetchError(esp_err_t err) {
  switch (err) {
    case ESP_ERR_TIMEOUT: return "ble agent timeout";
    case ESP_ERR_INVALID_STATE: return "ble agent disconnected";
    case ESP_ERR_INVALID_SIZE: return "response too large";
    default: return "ble agent error";
  }
}

}  // namespace

void setAgent(const Agent& agent) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_agent = agent;
}

void setMode(Mode mode) { g_mode = mode; }
Mode mode() { return g_mode.load(); }

bool agentLinked() {
  bool (*linked)() = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    linked = g_agent.linked;
  }
  return linked != nullptr && linked();
}

const char* active(bool wifi_connected) {
  const Mode m = mode();
  if (m != Mode::Wifi && agentLinked()) return "ble";
  if (m == Mode::Ble) return "none";
  return wifi_connected ? "wifi" : "none";
}

bool route(const NetRequest& req, NetResponse& resp) {
  const Mode m = mode();
  if (m == Mode::Wifi) return false;
  Agent agent;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    agent = g_agent;
  }
  const bool linked = agent.linked != nullptr && agent.fetch != nullptr && agent.linked();
  if (!linked) {
    if (m != Mode::Ble) return false;
    resp.status = -1;
    resp.error = "no BLE agent";
    return true;
  }

  Validator val;
  if (req.conditional_get) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_validators.find(req.url);
    if (it != g_validators.end()) val = it->second;
  }
  const std::string head = buildRequestHead(req, kMaxBodyBytes, val.etag, val.last_modified);
  std::string resp_head;
  uint8_t* body = nullptr;
  size_t body_len = 0;
  const esp_err_t err =
      agent.fetch(head, reinterpret_cast<const uint8_t*>(req.body.data()), req.body.size(),
                  req.timeout_ms + kGraceMs, resp_head, &body, &body_len);
  if (err != ESP_OK) {
    resp.status = -1;
    resp.error = fetchError(err);
    ESP_LOGW(TAG, "%s %s via agent: %s", req.method.c_str(), hostOf(req.url).c_str(),
             resp.error.c_str());
    return true;
  }
  int status = 0;
  std::string error;
  if (!parseResponseHead(resp_head, status, resp.headers, error) || status == 0) {
    if (body != nullptr) heap_caps_free(body);
    resp.status = -1;
    resp.error = error;
    ESP_LOGW(TAG, "%s %s via agent: %s", req.method.c_str(), hostOf(req.url).c_str(), error.c_str());
    return true;
  }
  resp.status = status;
  if (status == 304) {
    resp.not_modified = true;
    if (body != nullptr) heap_caps_free(body);
  } else {
    resp.adoptBody(reinterpret_cast<char*>(body), body_len);
    if (resp.ok() && req.conditional_get) {
      Validator v;
      if (const std::string* et = resp.header("etag")) v.etag = *et;
      if (const std::string* lm = resp.header("last-modified")) v.last_modified = *lm;
      if (!v.etag.empty() || !v.last_modified.empty()) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_validators.size() >= kMaxValidators && !g_validators.count(req.url)) {
          g_validators.erase(g_validators.begin());
        }
        g_validators[req.url] = std::move(v);
      }
    }
  }
  return true;
}

}  // namespace eego::net::uplink
