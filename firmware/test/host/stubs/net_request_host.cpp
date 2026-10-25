// SPDX-License-Identifier: GPL-3.0-or-later
// Host replacement for the one symbol of net_stack/src/http_pool.cpp that
// data_pipeline needs (http_pool.cpp itself depends on esp_http_client).
// Mirrors the firmware behaviour: header values are wiped on destruction.
#include "net_stack/http_pool.h"

namespace eego::net {

NetRequest::~NetRequest() {
  for (auto& header : headers) {
    volatile char* data = header.value.empty() ? nullptr : header.value.data();
    for (size_t i = 0; i < header.value.size(); ++i) data[i] = 0;
  }
}

}  // namespace eego::net
