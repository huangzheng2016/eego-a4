// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

#include "data_pipeline/types.h"
#include "net_stack/net_task.h"

namespace eego::dashboard::pipeline {

class NetStackTransport final : public IHttpTransport {
 public:
  explicit NetStackTransport(net::NetStack& stack, uint32_t wait_ms = 30000)
      : stack_(stack), wait_ms_(wait_ms) {}

  bool execute(const net::NetRequest& request, HttpResponse& response) override;

 private:
  net::NetStack& stack_;
  uint32_t wait_ms_;
};

TokenResolver makeNetStackTokenResolver();

}  // namespace eego::dashboard::pipeline
