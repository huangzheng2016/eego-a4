// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

//
//
//
//   auto r = eego::net::httpPushIngest(body, client_ip);
//   if (r.code == PushResult::Code::Ok) return httpd_resp_send_201_or_202(...);

#include <string>

#include "net_stack/net_task.h"

namespace eego::net {
inline NetStack::PushResult httpPushIngest(const std::string& body,
                                           const std::string& sender,
                                           uint32_t wait_ms = 5000) {
  return NetStack::instance().pushIngest(body, sender, wait_ms);
}

}  // namespace eego::net
