// SPDX-License-Identifier: GPL-3.0-or-later
#include "data_pipeline/net_stack_transport.h"

#include <algorithm>
#include <limits>

#include "net_stack/credentials.h"

namespace eego::dashboard::pipeline {

bool NetStackTransport::execute(const net::NetRequest& request, HttpResponse& response) {
  response = HttpResponse{};
  net::NetResponse incoming;
  const uint64_t requested_wait = static_cast<uint64_t>(request.timeout_ms) + 5000;
  const uint32_t wait_ms = requested_wait > std::numeric_limits<uint32_t>::max()
                               ? std::numeric_limits<uint32_t>::max()
                               : std::max(wait_ms_, static_cast<uint32_t>(requested_wait));
  if (!stack_.httpSubmit(request, &incoming, wait_ms)) {
    response.status = -1;
    response.error = "net_task queue unavailable or timed out";
    return false;
  }
  response.status = incoming.status;
  response.not_modified = incoming.not_modified;
  response.error = incoming.error;
  response.headers = incoming.headers;
  response.body.assign(incoming.data(), incoming.length());
  return incoming.status != -1;
}

TokenResolver makeNetStackTokenResolver() {
  return [](const std::string& ref) { return net::CredentialStore::resolveToken(ref); };
}

}  // namespace eego::dashboard::pipeline
