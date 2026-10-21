// SPDX-License-Identifier: GPL-3.0-or-later
// KeyedPool 会话池实现 —— 本翻译单元是 net_stack 内唯一调用
// esp_http_client 的地方（net_task 单属主，M12 定稿）。
#include "net_stack/http_pool.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace eego::net {

static const char* TAG = "net.pool";
static const char* kUserAgent = "eego-a4-alert-panel/1.0";
// 小响应阈值：不超过此值直接放 std::string，超过则走 PSRAM 缓冲
constexpr size_t kSmallBodyLimit = 4096;

// ---------------------------------------------------------------------------
// URL 解析（仅支持 http/https://host[:port]/path?query，不支持 userinfo）
// ---------------------------------------------------------------------------
struct ParsedUrl {
  std::string scheme;
  std::string host;
  uint16_t port = 0;
  std::string path;  // "/path?query"，空则为 "/"
  bool valid = false;
};

static ParsedUrl parseUrl(const std::string& url) {
  ParsedUrl u;
  size_t scheme_end = url.find("://");
  if (scheme_end == std::string::npos) return u;
  u.scheme = url.substr(0, scheme_end);
  if (u.scheme != "http" && u.scheme != "https") return u;
  size_t host_start = scheme_end + 3;
  size_t path_start = url.find_first_of("/?", host_start);
  std::string hostport = url.substr(
      host_start, path_start == std::string::npos ? std::string::npos : path_start - host_start);
  if (hostport.empty()) return u;
  size_t colon = hostport.find(':');
  if (colon != std::string::npos) {
    u.host = hostport.substr(0, colon);
    long p = strtol(hostport.c_str() + colon + 1, nullptr, 10);
    if (p <= 0 || p > 65535) return u;
    u.port = (uint16_t)p;
  } else {
    u.host = hostport;
    u.port = (u.scheme == "https") ? 443 : 80;
  }
  if (path_start == std::string::npos) {
    u.path = "/";
  } else if (url[path_start] == '?') {
    u.path = "/" + url.substr(path_start);
  } else {
    u.path = url.substr(path_start);
  }
  u.valid = !u.host.empty();
  return u;
}

// ---------------------------------------------------------------------------
// NetResponse 所有权
// ---------------------------------------------------------------------------
NetResponse::~NetResponse() {
  if (psbuf_) heap_caps_free(psbuf_);
}

NetResponse::NetResponse(NetResponse&& o) noexcept
    : status(o.status),
      not_modified(o.not_modified),
      error(std::move(o.error)),
      headers(std::move(o.headers)),
      body_(std::move(o.body_)),
      psbuf_(o.psbuf_),
      pslen_(o.pslen_) {
  o.psbuf_ = nullptr;
  o.pslen_ = 0;
}

NetResponse& NetResponse::operator=(NetResponse&& o) noexcept {
  if (this != &o) {
    if (psbuf_) heap_caps_free(psbuf_);
    status = o.status;
    not_modified = o.not_modified;
    error = std::move(o.error);
    headers = std::move(o.headers);
    body_ = std::move(o.body_);
    psbuf_ = o.psbuf_;
    pslen_ = o.pslen_;
    o.psbuf_ = nullptr;
    o.pslen_ = 0;
  }
  return *this;
}

const std::string* NetResponse::header(const std::string& lower_name) const {
  auto it = headers.find(lower_name);
  return it == headers.end() ? nullptr : &it->second;
}

// ---------------------------------------------------------------------------
// HttpPool::Impl
// ---------------------------------------------------------------------------
struct HttpPool::Impl {
  explicit Impl(const PoolConfig& c) : cfg(c) {}

  PoolConfig cfg;

  // 接收缓冲（每个会话一份；event_handler 经 user_data 绑定）
  struct RxBuf {
    std::string small;   // ≤ kSmallBodyLimit 时的累积缓冲
    char* ps = nullptr;  // PSRAM 缓冲
    size_t len = 0;
    size_t cap = 0;
    size_t init_cap = 64 * 1024;   // 来自 PoolConfig.rx_buf_init
    size_t max_cap = 512 * 1024;   // 来自 PoolConfig.rx_buf_max
    bool use_ps = false;
    bool overflow = false;
    std::map<std::string, std::string>* headers = nullptr;
  };

  struct Session {
    esp_http_client_handle_t handle = nullptr;
    std::string key;       // scheme://host:port#verify
    std::string host;      // 纯主机名（esp_http_client 的 host 不含端口）
    std::string scheme;
    uint16_t port = 0;
    bool tls_verify = true;
    int64_t last_used_ms = 0;
    RxBuf rx;
  };

  // 条件 GET 校验器：host+path → ETag/Last-Modified（会话重建后仍有效）
  struct Validator {
    std::string etag;
    std::string last_modified;
  };
  std::map<std::string, Validator> validators_;

  // host 级节流：传输失败冷却 + 服务器拒绝分级退避
  struct HostState {
    int64_t transport_cooldown_until_ms = 0;
    int64_t server_cooldown_until_ms = 0;
    uint32_t server_backoff_level = 0;
  };
  std::map<std::string, HostState> hosts_;

  std::vector<std::unique_ptr<Session>> sessions_;

  static int64_t nowMs() { return esp_timer_get_time() / 1000; }

  std::string sessionKey(const ParsedUrl& u, bool tls_verify) const {
    return u.scheme + "://" + u.host + ":" + std::to_string(u.port) +
           (tls_verify ? "#v" : "#nv");
  }

  std::string hostKey(const ParsedUrl& u) const {
    return u.scheme + "://" + u.host + ":" + std::to_string(u.port);
  }

  // 条件 GET 校验器键：scheme://host:port + path（跨会话/重建有效，http/https 不串）
  std::string validatorKey(const Session& s, const std::string& path) const {
    return s.scheme + "://" + s.host + ":" + std::to_string(s.port) + path;
  }

  Session* findSession(const std::string& key) {
    for (auto& s : sessions_)
      if (s->key == key) return s.get();
    return nullptr;
  }

  void closeSession(Session* s) {
    if (s->handle) esp_http_client_cleanup(s->handle);
    s->handle = nullptr;
    if (s->rx.ps) {
      heap_caps_free(s->rx.ps);
      s->rx.ps = nullptr;
      s->rx.cap = s->rx.len = 0;
    }
  }

  void evictLruIfFull() {
    if (sessions_.size() < cfg.max_sessions) return;
    // 单属主模型下无"使用中"会话（execute 顺序执行），直接关最旧
    auto oldest = std::min_element(
        sessions_.begin(), sessions_.end(),
        [](const std::unique_ptr<Session>& a, const std::unique_ptr<Session>& b) {
          return a->last_used_ms < b->last_used_ms;
        });
    if (oldest != sessions_.end()) {
      ESP_LOGI(TAG, "pool full, evict LRU session %s", (*oldest)->key.c_str());
      closeSession(oldest->get());
      sessions_.erase(oldest);
    }
  }

  bool openSession(Session* s) {
    esp_http_client_config_t cfg = {};
    cfg.host = s->host.c_str();
    cfg.port = s->port;
    cfg.path = "/";
    cfg.transport_type =
        s->scheme == "https" ? HTTP_TRANSPORT_OVER_SSL : HTTP_TRANSPORT_OVER_TCP;
    if (s->scheme == "https" && s->tls_verify) cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 20000;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 2048;
    cfg.keep_alive_enable = true;
    cfg.event_handler = &Impl::httpEventHandler;
    cfg.user_data = &s->rx;
    s->rx.init_cap = this->cfg.rx_buf_init;
    s->rx.max_cap = this->cfg.rx_buf_max;
    s->handle = esp_http_client_init(&cfg);
    if (!s->handle) {
      ESP_LOGE(TAG, "session init failed: %s", s->key.c_str());
      return false;
    }
    return true;
  }

  static esp_err_t httpEventHandler(esp_http_client_event_t* evt) {
    RxBuf* rx = static_cast<RxBuf*>(evt->user_data);
    switch (evt->event_id) {
      case HTTP_EVENT_ON_DATA:
        if (rx && evt->data_len > 0) {
          if (rx->overflow) break;  // 已判溢出：丢弃后续数据，绝不静默累积
          if (rx->use_ps) {
            size_t need = rx->len + evt->data_len;
            if (need > rx->cap) {
              size_t newcap = rx->cap ? rx->cap : 1024;
              while (newcap < need && newcap < rx->max_cap) newcap *= 2;
              if (newcap > rx->max_cap) newcap = rx->max_cap;
              if (newcap >= need) {
                char* nb = (char*)heap_caps_realloc(rx->ps, newcap, MALLOC_CAP_SPIRAM);
                if (nb) {
                  rx->ps = nb;
                  rx->cap = newcap;
                } else {
                  ESP_LOGE(TAG, "PSRAM grow to %u KB failed",
                           (unsigned)(newcap / 1024));
                }
              }
            }
            if (rx->len + evt->data_len <= rx->cap) {
              memcpy(rx->ps + rx->len, evt->data, evt->data_len);
              rx->len += evt->data_len;
            } else {
              rx->overflow = true;  // 到顶仍放不下：报错，绝不静默截断
            }
          } else {
            rx->small.append((const char*)evt->data, evt->data_len);
            if (rx->small.size() > kSmallBodyLimit) {
              // 小响应升级 PSRAM（移动已有字节）
              rx->use_ps = true;
              rx->cap = rx->init_cap;
              rx->ps = (char*)heap_caps_malloc(rx->cap, MALLOC_CAP_SPIRAM);
              if (rx->ps) {
                memcpy(rx->ps, rx->small.data(), rx->small.size());
                rx->len = rx->small.size();
                rx->small.clear();
                rx->small.shrink_to_fit();
              } else {
                rx->use_ps = false;
                rx->overflow = true;
              }
            }
          }
        }
        break;
      case HTTP_EVENT_ON_HEADER:
        if (rx && rx->headers && evt->header_key && evt->header_value) {
          std::string k = evt->header_key;
          std::transform(k.begin(), k.end(), k.begin(),
                         [](unsigned char c) { return std::tolower(c); });
          auto it = rx->headers->find(k);
          if (k == "set-cookie" && it != rx->headers->end())
            it->second += "\n" + std::string(evt->header_value);
          else
            (*rx->headers)[k] = evt->header_value;
        }
        break;
      default:
        break;
    }
    return ESP_OK;
  }

  // 单次执行（不重建）。返回的 resp.status==-1 表示传输级失败。
  NetResponse executeOnce(Session* s, const NetRequest& req, const ParsedUrl& u) {
    NetResponse resp;
    s->rx.headers = &resp.headers;
    s->rx.small.clear();
    s->rx.len = 0;
    s->rx.overflow = false;
    if (!s->rx.use_ps && s->rx.ps) { /* 不应发生 */ }

    esp_http_client_set_url(s->handle, u.path.c_str());
    esp_http_client_set_timeout_ms(s->handle, (int)req.timeout_ms);
    esp_http_client_set_method(s->handle,
                               req.method == "POST"   ? HTTP_METHOD_POST
                               : req.method == "HEAD" ? HTTP_METHOD_HEAD
                                                      : HTTP_METHOD_GET);
    esp_http_client_set_header(s->handle, "User-Agent", kUserAgent);
    esp_http_client_set_header(s->handle, "Accept", "application/json, */*");
    for (const auto& h : req.headers) {
      esp_http_client_set_header(s->handle, h.name.c_str(), h.value.c_str());
    }

    if (req.conditional_get) {
      auto it = validators_.find(validatorKey(*s, u.path));
      if (it != validators_.end()) {
        if (!it->second.etag.empty())
          esp_http_client_set_header(s->handle, "If-None-Match", it->second.etag.c_str());
        if (!it->second.last_modified.empty())
          esp_http_client_set_header(s->handle, "If-Modified-Since",
                                     it->second.last_modified.c_str());
      }
    }
    if (req.method == "POST") {
      esp_http_client_set_header(s->handle, "Content-Type", "application/json");
      esp_http_client_set_post_field(s->handle, req.body.data(), req.body.size());
    } else {
      esp_http_client_set_post_field(s->handle, nullptr, 0);
    }

    esp_err_t err = esp_http_client_perform(s->handle);
    s->last_used_ms = nowMs();
    if (err != ESP_OK) {
      resp.error = esp_err_to_name(err);
      resp.status = -1;
      return resp;
    }
    resp.status = esp_http_client_get_status_code(s->handle);

    // 完整性校验：Content-Length vs 实收（chunked 为 -1，跳过）
    int64_t clen = esp_http_client_get_content_length(s->handle);
    size_t received = s->rx.use_ps ? s->rx.len : s->rx.small.size();
    if (resp.status > 0 && clen > 0 && (int64_t)received < clen) {
      ESP_LOGW(TAG, "incomplete response: %u/%lld bytes, host=%s", (unsigned)received,
               (long long)clen, s->host.c_str());
      resp.error = "incomplete response";
      resp.status = -1;
      return resp;
    }
    if (s->rx.overflow) {
      ESP_LOGW(TAG, "response overflow >%uKB, host=%s",
               (unsigned)(s->rx.max_cap / 1024), s->host.c_str());
      resp.error = "response too large";
      resp.status = -1;
      return resp;
    }

    // 条件 GET 校验器更新（2xx 才更新；304 沿用旧值）
    if (resp.status == 304) {
      resp.not_modified = true;
    } else if (resp.ok()) {
      Validator v;
      if (const std::string* et = resp.header("etag")) v.etag = *et;
      if (const std::string* lm = resp.header("last-modified")) v.last_modified = *lm;
      if (!v.etag.empty() || !v.last_modified.empty())
        validators_[validatorKey(*s, u.path)] = std::move(v);
    }

    // body 所有权转移
    if (s->rx.use_ps && s->rx.ps) {
      if (s->rx.len < s->rx.cap) s->rx.ps[s->rx.len] = '\0';
      resp.psbuf_ = s->rx.ps;
      resp.pslen_ = s->rx.len;
      s->rx.ps = nullptr;
      s->rx.cap = s->rx.len = 0;
      s->rx.use_ps = false;
    } else {
      resp.body_ = std::move(s->rx.small);
    }
    return resp;
  }
};

// ---------------------------------------------------------------------------
// HttpPool 公共接口
// ---------------------------------------------------------------------------
HttpPool::HttpPool(const PoolConfig& cfg) : impl_(std::make_unique<Impl>(cfg)) {}
HttpPool::~HttpPool() {
  for (auto& s : impl_->sessions_) impl_->closeSession(s.get());
}

NetResponse HttpPool::execute(const NetRequest& req) {
  NetResponse resp;
  ParsedUrl u = parseUrl(req.url);
  if (!u.valid) {
    resp.error = "invalid url";
    resp.status = -1;
    return resp;
  }
  const std::string hkey = impl_->hostKey(u);
  const int64_t now = Impl::nowMs();

  // 主机级节流检查（传输冷却 / 服务器退避）
  auto hit = impl_->hosts_.find(hkey);
  if (hit != impl_->hosts_.end()) {
    const Impl::HostState& hs = hit->second;
    if (hs.transport_cooldown_until_ms > now) {
      resp.error = "host cooldown (transport)";
      resp.status = -1;
      return resp;
    }
    if (hs.server_cooldown_until_ms > now) {
      resp.error = "host backoff (server)";
      resp.status = -1;
      return resp;
    }
  }

  const std::string skey = impl_->sessionKey(u, req.tls_verify);
  Impl::Session* s = impl_->findSession(skey);
  if (!s) {
    impl_->evictLruIfFull();
    auto ns = std::make_unique<Impl::Session>();
    ns->key = skey;
    ns->host = u.host;  // 纯主机名（端口走 esp_http_client_config_t.port）
    ns->scheme = u.scheme;
    ns->port = u.port;
    ns->tls_verify = req.tls_verify;
    s = ns.get();
    impl_->sessions_.push_back(std::move(ns));
  }
  if (!s->handle && !impl_->openSession(s)) {
    resp.error = "session init failed";
    resp.status = -1;
    // 从池中移除失败会话
    for (auto it = impl_->sessions_.begin(); it != impl_->sessions_.end(); ++it) {
      if (it->get() == s) {
        impl_->sessions_.erase(it);
        break;
      }
    }
    return resp;
  }

  resp = impl_->executeOnce(s, req, u);

  // 传输级失败（非 overflow/不完整，那些已作废弃处理）：重建会话重试一次
  if (resp.status == -1 && !s->rx.overflow) {
    ESP_LOGW(TAG, "request failed (%s), rebuild session and retry once: %s",
             resp.error.c_str(), skey.c_str());
    impl_->closeSession(s);
    if (impl_->openSession(s)) {
      resp = impl_->executeOnce(s, req, u);
    } else {
      resp.error = "session rebuild failed";
      resp.status = -1;
    }
  }

  // 会话作废条件：overflow / 响应不完整 / 传输重试后仍失败
  if (s->rx.overflow ||
      (resp.status == -1 && resp.error != "host cooldown (transport)" &&
       resp.error != "host backoff (server)")) {
    impl_->closeSession(s);
    for (auto it = impl_->sessions_.begin(); it != impl_->sessions_.end(); ++it) {
      if (it->get() == s) {
        impl_->sessions_.erase(it);
        break;
      }
    }
    // 主机冷却 30s（传输失败）
    impl_->hosts_[hkey].transport_cooldown_until_ms = now + impl_->cfg.host_cooldown_ms;
    return resp;
  }

  // 服务器拒绝分级退避：429/5xx → Retry-After 优先，否则 30s 起翻倍封顶 300s
  if (resp.status == 429 || resp.status >= 500) {
    Impl::HostState& hs = impl_->hosts_[hkey];
    int64_t cooldown = 30000;
    if (const std::string* ra = resp.header("retry-after")) {
      long secs = strtol(ra->c_str(), nullptr, 10);
      if (secs > 0) cooldown = std::min<int64_t>(secs * 1000, 300000);
    } else {
      hs.server_backoff_level = std::min(hs.server_backoff_level + 1, (uint32_t)4);
      cooldown = std::min<int64_t>(30000ll << (hs.server_backoff_level - 1), 300000);
    }
    hs.server_cooldown_until_ms = now + cooldown;
    ESP_LOGW(TAG, "host %s server backoff %lldms (status=%d)", hkey.c_str(),
             (long long)cooldown, resp.status);
  } else if (resp.status > 0) {
    impl_->hosts_[hkey].server_backoff_level = 0;  // 收到正常响应即复位
  }
  return resp;
}

void HttpPool::housekeeping(int64_t now_ms) {
  for (auto it = impl_->sessions_.begin(); it != impl_->sessions_.end();) {
    Impl::Session* s = it->get();
    if (s->last_used_ms > 0 && now_ms - s->last_used_ms > (int64_t)impl_->cfg.idle_close_ms) {
      ESP_LOGI(TAG, "session idle %lldms, close %s", (long long)(now_ms - s->last_used_ms),
               s->key.c_str());
      impl_->closeSession(s);
      it = impl_->sessions_.erase(it);
    } else {
      ++it;
    }
  }
}

bool HttpPool::hostThrottled(const std::string& url) const {
  ParsedUrl u = parseUrl(url);
  if (!u.valid) return false;
  auto it = impl_->hosts_.find(impl_->hostKey(u));
  if (it == impl_->hosts_.end()) return false;
  const int64_t now = Impl::nowMs();
  return it->second.transport_cooldown_until_ms > now ||
         it->second.server_cooldown_until_ms > now;
}

size_t HttpPool::sessionCount() const { return impl_->sessions_.size(); }

}  // namespace eego::net
