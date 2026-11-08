// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

//
//
//

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace eego::net {
struct NetHeader {
  std::string name;
  std::string value;
};

struct NetRequest {
  std::string method = "GET";  // GET / POST / HEAD
  std::string url;
  std::vector<NetHeader> headers;
  std::string body;
  uint32_t timeout_ms = 20000;
  bool tls_verify = true;
  bool conditional_get = false;

  ~NetRequest();
};

struct NetResponse {
  int status = 0;
  bool not_modified = false;
  std::string error;
  std::map<std::string, std::string> headers;

  NetResponse() = default;
  ~NetResponse();
  NetResponse(NetResponse&& o) noexcept;
  NetResponse& operator=(NetResponse&& o) noexcept;
  NetResponse(const NetResponse&) = delete;
  NetResponse& operator=(const NetResponse&) = delete;

  bool ok() const { return status >= 200 && status < 300; }
  const char* data() const { return psbuf_ ? psbuf_ : body_.data(); }
  size_t length() const { return psbuf_ ? pslen_ : body_.size(); }
  const std::string* header(const std::string& lower_name) const;

 private:
  friend class HttpPool;
  std::string body_;
  char* psbuf_ = nullptr;
  size_t pslen_ = 0;
};

struct PoolConfig {
  size_t max_sessions = 2;
  uint32_t idle_close_ms = 10000;
  uint32_t rx_buf_init = 64 * 1024;
  uint32_t rx_buf_max = 512 * 1024;
  uint32_t host_cooldown_ms = 30000;
};

class HttpPool {
 public:
  explicit HttpPool(const PoolConfig& cfg = PoolConfig());
  ~HttpPool();
  HttpPool(const HttpPool&) = delete;
  HttpPool& operator=(const HttpPool&) = delete;

  NetResponse execute(const NetRequest& req);

  void housekeeping(int64_t now_ms);

  size_t sessionCount() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace eego::net
