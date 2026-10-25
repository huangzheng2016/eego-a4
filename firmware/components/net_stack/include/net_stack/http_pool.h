// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// NetRequest/NetResponse 与 KeyedPool 每 host keep-alive 会话池（M12 定稿）。
//
// 并发合同：HttpPool 只能在 net_task 上下文使用 —— net_task 是一切
// esp_http_client 的唯一属主，本组件是唯一调用 esp_http_client 的翻译单元。
//
// 会话策略（M12 §3.3）：
//   * KeyedPool<scheme://host:port, Session>，上限 4 会话 LRU；
//   * 每 host keep-alive：open 一次，之后 set_url 换路径复用；
//   * perform 失败（传输级）→ close + 重建 + 重试一次；仍失败 → 该 host 冷却 30s；
//   * HTTP 429/5xx → 分级退避（Retry-After 优先，否则 30s 起翻倍、封顶 300s），
//     退避期内对该 host 的请求快速失败（由源级退避叠加，不 hammer 对端）；
//   * 闲置 > 30s 关闭；
//   * RX 缓冲 PSRAM 64KB 起、翻倍至 512KB 封顶；overflow 即报错绝不静默截断
//     （weread PROGRESS §15 血泪），overflow 后会话作废重建；
//   * Content-Length 完整性校验（chunked 跳过）；
//   * ETag/Last-Modified 条件 GET（per host+path 校验器，会话重建后仍有效）。
//
// TLS verify 开关：verify=true 挂 esp_crt_bundle_attach（sdkconfig 已开
// CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL）；verify=false 不挂任何 CA，
// esp-tls 自动 MBEDTLS_SSL_VERIFY_NONE（esp_tls_mbedtls.c 实证）——
// 仅用于内网自签源，配置侧必须显式关闭。

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

// 出站请求描述。调用方填充，net_task 经 HttpPool 执行。
struct NetRequest {
  std::string method = "GET";  // GET / POST / HEAD
  std::string url;             // http:// 或 https:// 完整 URL
  std::vector<NetHeader> headers;
  std::string body;            // POST 时有效
  uint32_t timeout_ms = 20000;
  bool tls_verify = true;
  bool conditional_get = false;  // true 且持有校验器时携带 If-None-Match/If-Modified-Since

  ~NetRequest();
};

// 响应。body 超过阈值时驻留 PSRAM（move-only，禁止拷贝防 double-free）。
// headers 的 key 为小写；set-cookie 多条以 '\n' 拼接（weread 惯例）。
struct NetResponse {
  int status = 0;           // HTTP 状态码；传输级失败 = -1
  bool not_modified = false;  // 304（条件 GET 命中）
  std::string error;        // 传输/完整性错误描述（不含秘密）
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
  // 响应头查找（key 需小写）；不存在返回 nullptr
  const std::string* header(const std::string& lower_name) const;

 private:
  friend class HttpPool;
  std::string body_;   // 小响应（≤ 阈值）
  char* psbuf_ = nullptr;  // PSRAM 大响应（所有权随 move 转移）
  size_t pslen_ = 0;
};

struct PoolConfig {
  size_t max_sessions = 4;          // KeyedPool 上限（M12）
  uint32_t idle_close_ms = 30000;   // 闲置关闭
  uint32_t rx_buf_init = 64 * 1024;   // PSRAM RX 缓冲初始（M12）
  uint32_t rx_buf_max = 512 * 1024;   // PSRAM RX 缓冲封顶（M12）
  uint32_t host_cooldown_ms = 30000;  // 传输失败主机冷却
};

class HttpPool {
 public:
  explicit HttpPool(const PoolConfig& cfg = PoolConfig());
  ~HttpPool();
  HttpPool(const HttpPool&) = delete;
  HttpPool& operator=(const HttpPool&) = delete;

  // 执行一次请求。仅 net_task 上下文可调用。
  // 传输失败自动重建会话重试一次；冷却/退避期内快速失败（error 说明原因）。
  NetResponse execute(const NetRequest& req);

  // 闲置会话关闭；net_task 每次循环调用。
  void housekeeping(int64_t now_ms);

  size_t sessionCount() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace eego::net
