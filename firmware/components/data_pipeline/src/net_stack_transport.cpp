// SPDX-License-Identifier: GPL-3.0-or-later
#include "data_pipeline/net_stack_transport.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <new>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "net_stack/credentials.h"

namespace eego::dashboard::pipeline {
namespace {

constexpr UBaseType_t kRequestQueueLength = 16;
constexpr UBaseType_t kResultQueueLength = kRequestQueueLength + 1;
constexpr uint32_t kWorkerStackBytes = 8192;
constexpr UBaseType_t kWorkerPriority = 3;

struct AsyncExchange {
  uint64_t id = 0;
  net::NetRequest request;
  AsyncHttpResult result;
};

void performRequest(net::NetStack& stack, uint32_t wait_ms, const net::NetRequest& request,
                    HttpResponse& response) {
  response = HttpResponse{};
  net::NetResponse incoming;
  const uint64_t requested_wait = static_cast<uint64_t>(request.timeout_ms) + 5000;
  const uint32_t bounded_wait = requested_wait > std::numeric_limits<uint32_t>::max()
                                    ? std::numeric_limits<uint32_t>::max()
                                    : std::max(wait_ms, static_cast<uint32_t>(requested_wait));
  if (!stack.httpSubmit(request, &incoming, bounded_wait)) {
    response.status = -1;
    response.error = "net_task queue unavailable or timed out";
    return;
  }
  response.status = incoming.status;
  response.not_modified = incoming.not_modified;
  response.error = incoming.error;
  response.headers = incoming.headers;
  response.body.assign(incoming.data(), incoming.length());
}

}  // namespace

struct NetStackTransport::AsyncState {
  AsyncState(net::NetStack& net, uint32_t wait)
      : stack(net), wait_ms(wait), requests(xQueueCreate(kRequestQueueLength, sizeof(AsyncExchange*))),
        results(xQueueCreate(kResultQueueLength, sizeof(AsyncExchange*))) {}

  ~AsyncState() {
    AsyncExchange* exchange = nullptr;
    if (requests != nullptr) {
      while (xQueueReceive(requests, &exchange, 0) == pdTRUE) delete exchange;
      vQueueDelete(requests);
    }
    if (results != nullptr) {
      while (xQueueReceive(results, &exchange, 0) == pdTRUE) delete exchange;
      vQueueDelete(results);
    }
  }

  static void start(const std::shared_ptr<AsyncState>& state) {
    if (state->requests == nullptr || state->results == nullptr) return;
    auto* holder = new (std::nothrow) std::shared_ptr<AsyncState>(state);
    if (holder == nullptr) return;
    if (xTaskCreate(&workerEntry, "dashboard_http", kWorkerStackBytes / sizeof(StackType_t), holder,
                    kWorkerPriority, nullptr) != pdPASS) {
      delete holder;
      return;
    }
    state->started = true;
  }

  static void workerEntry(void* argument) {
    auto* holder = static_cast<std::shared_ptr<AsyncState>*>(argument);
    std::shared_ptr<AsyncState> state = *holder;
    delete holder;
    state->run();
    state->started = false;
    state.reset();
    vTaskDelete(nullptr);
  }

  void run() {
    while (!stopping) {
      AsyncExchange* exchange = nullptr;
      if (xQueueReceive(requests, &exchange, portMAX_DELAY) != pdTRUE) continue;
      if (exchange == nullptr || stopping) {
        delete exchange;
        break;
      }
      exchange->result.id = exchange->id;
      performRequest(stack, wait_ms, exchange->request, exchange->result.response);
      if (xQueueSend(results, &exchange, 0) != pdTRUE) {
        --pending;
        delete exchange;
      }
    }
    AsyncExchange* exchange = nullptr;
    while (xQueueReceive(requests, &exchange, 0) == pdTRUE) {
      if (exchange != nullptr) --pending;
      delete exchange;
    }
  }

  void stop() {
    stopping = true;
    if (requests != nullptr) {
      AsyncExchange* wake = nullptr;
      xQueueSend(requests, &wake, 0);
    }
  }

  net::NetStack& stack;
  uint32_t wait_ms;
  QueueHandle_t requests = nullptr;
  QueueHandle_t results = nullptr;
  std::atomic<size_t> pending{0};
  std::atomic<bool> stopping{false};
  std::atomic<bool> started{false};
};

NetStackTransport::NetStackTransport(net::NetStack& stack, uint32_t wait_ms)
    : stack_(stack), wait_ms_(wait_ms), async_(std::make_shared<AsyncState>(stack, wait_ms)) {
  AsyncState::start(async_);
}

NetStackTransport::~NetStackTransport() {
  if (async_ != nullptr) {
    async_->stop();
    async_.reset();
  }
}

bool NetStackTransport::execute(const net::NetRequest& request, HttpResponse& response) {
  performRequest(stack_, wait_ms_, request, response);
  return response.status != -1;
}

bool NetStackTransport::executeAsync(uint64_t id, const net::NetRequest& request) {
  if (async_ == nullptr || !async_->started || async_->stopping) return false;
  auto* exchange = new (std::nothrow) AsyncExchange;
  if (exchange == nullptr) return false;
  exchange->id = id;
  exchange->request = request;
  ++async_->pending;
  if (xQueueSend(async_->requests, &exchange, 0) != pdTRUE) {
    --async_->pending;
    delete exchange;
    return false;
  }
  return true;
}

bool NetStackTransport::pollAsync(AsyncHttpResult& result) {
  if (async_ == nullptr || async_->results == nullptr) return false;
  AsyncExchange* exchange = nullptr;
  if (xQueueReceive(async_->results, &exchange, 0) != pdTRUE) return false;
  if (exchange != nullptr) {
    result = std::move(exchange->result);
    --async_->pending;
    delete exchange;
  }
  return true;
}

bool NetStackTransport::asyncIdle() const {
  return async_ == nullptr || async_->pending == 0;
}

TokenResolver makeNetStackTokenResolver() {
  return [](const std::string& ref) { return net::CredentialStore::resolveToken(ref); };
}

}  // namespace eego::dashboard::pipeline
