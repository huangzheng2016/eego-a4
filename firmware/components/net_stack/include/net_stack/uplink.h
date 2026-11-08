// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Uplink selection for outbound HTTP (data sources, probes). Requests normally go over
// WiFi through HttpPool; when a computer agent (eego-link) is linked over BLE they can
// be carried by the agent instead, which performs the request (including TLS) on the
// computer. The BLE side is injected (net_stack must not depend on the ble component).

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

#include "esp_err.h"
#include "net_stack/http_pool.h"

namespace eego::net::uplink {

enum class Mode : uint8_t {
  Auto = 0,  // agent when linked, otherwise WiFi
  Wifi = 1,  // never use the agent
  Ble = 2,   // agent only; fail when no agent is linked
};

const char* modeName(Mode mode);

struct Agent {
  bool (*linked)() = nullptr;
  // On ESP_OK resp_body is heap_caps memory owned by the caller (may be nullptr when empty).
  esp_err_t (*fetch)(const std::string& head, const uint8_t* body, size_t body_len,
                     uint32_t wait_ms, std::string& resp_head, uint8_t** resp_body,
                     size_t* resp_body_len) = nullptr;
};

// Largest response body requested from the agent (fits the BLE tunnel's 384 KiB limit).
constexpr uint32_t kMaxBodyBytes = 380 * 1024;
// Extra wait on top of the request timeout before the device gives up on the agent.
constexpr uint32_t kGraceMs = 5000;

void setAgent(const Agent& agent);
void setMode(Mode mode);
Mode mode();
bool agentLinked();

// "ble" when requests go to the agent, "wifi" when WiFi carries them, "none" otherwise.
const char* active(bool wifi_connected);

// Handles the request when the uplink is the agent (or agent-only mode has no agent).
// Returns false when the caller should use WiFi.
bool route(const NetRequest& req, NetResponse& resp);

// Pure helpers (host-tested).
std::string buildRequestHead(const NetRequest& req, uint32_t max_body,
                             const std::string& if_none_match, const std::string& if_modified_since);
bool parseResponseHead(const std::string& head, int& status,
                       std::map<std::string, std::string>& headers, std::string& error);

}  // namespace eego::net::uplink
