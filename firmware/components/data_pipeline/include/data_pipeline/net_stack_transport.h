// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>

#include "data_pipeline/types.h"
#include "net_stack/net_task.h"

namespace eego::dashboard::pipeline {

class NetStackTransport final : public IHttpTransport {
 public:
  explicit NetStackTransport(net::NetStack& stack, uint32_t wait_ms = 30000);
  ~NetStackTransport() override;

  bool execute(const net::NetRequest& request, HttpResponse& response) override;
  bool asyncSupported() const override { return true; }
  bool executeAsync(uint64_t id, const net::NetRequest& request) override;
  bool pollAsync(AsyncHttpResult& result) override;
  bool asyncIdle() const override;
  void cancelAsync() override;

 private:
  struct AsyncState;
  net::NetStack& stack_;
  uint32_t wait_ms_;
  std::shared_ptr<AsyncState> async_;
};

TokenResolver makeNetStackTokenResolver();

}  // namespace eego::dashboard::pipeline
