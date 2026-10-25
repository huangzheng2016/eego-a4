// SPDX-License-Identifier: GPL-3.0-or-later
// 管理 HTTP API 服务器 —— 路由表、统一错误信封、Host/Origin 白名单、
// Bearer/X-Ingest-Key 认证、请求体预算、限速、全部端点 handlers。
//
// 并发边界（M12 定稿，强制）：本文件一切 handler 运行在 httpd 任务上下文，
// 只做解析/校验/限速/入队：
//   * 配置持久化 / 恢复出厂 / 重启 / WiFi 落 NVS → AdminCommand → app_task；
//   * 告警摄入 → NetStack 命令队列 → net_task（dashboard 出网测试见 dashboard_api）；
//   * 告警读 → SnapshotPublisher 快照（未接线 → 503）；
//   * 告警写（ack/clear）→ AlertActionSink（未接线 → 503）。
//
// 安全约定（M13 定稿）：
//   * 统一错误信封 {"error":{"code","message","fields"?}}；
//   * 401 同形不区分原因 + WWW-Authenticate: Bearer；
//   * Host/Origin 白名单（本机 IP/主机名），其余 403（抗 DNS rebinding）；
//   * 无任何 Access-Control-Allow-* 头（无 CORS）；非 JSON → 415；
//   * 一切未知路径（含 /api/v1/firmware/*）→ 404 JSON（首版无远程 OTA）；
//   * secrets 边界：响应/日志不出现 WiFi 密码、管理密码、token/key 全文
//     （ingest key 仅轮换响应完整显示一次）。

#include "admin/admin.h"
#include "admin/display_snapshot.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <strings.h>
#include <utility>
#include <vector>

#include "admin_internal.h"
#include "admin/input_bindings.h"
#include "ble/ble_manager.h"
#include "dashboard_api.h"
#include "dashboard_path.h"
#include "loader_page.h"
#include "device_settings/device_settings.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#include "net_stack/http_push_bridge.h"
#include "net_stack/net_task.h"
#include "net_stack/wifi_manager.h"
#include "admin/provision_portal.h"
#include "sd_card.h"

namespace eego::admin {

namespace {

// ---------------------------------------------------------------------------
// 服务器状态
// ---------------------------------------------------------------------------
httpd_handle_t g_server = nullptr;
AdminConfig g_cfg;

constexpr size_t kBodyBudgetLogin = 1024;
constexpr size_t kBodyBudgetConfig = 4 * 1024;   // M13：config ≤4KB
constexpr size_t kBodyBudgetAlerts = 64 * 1024;  // 资源预算（M13 T6）：低于批量
                                                 // 理论上限 512KB，见任务报告
constexpr size_t kBodyBudgetSmall = 8 * 1024;
constexpr size_t kBodyBudgetConfirm = 512;

// ---------------------------------------------------------------------------
// 响应工具
// ---------------------------------------------------------------------------
const char* statusText(int status) {
  switch (status) {
    case 200: return "200 OK";
    case 201: return "201 Created";
    case 202: return "202 Accepted";
    case 204: return "204 No Content";
    case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    case 403: return "403 Forbidden";
    case 404: return "404 Not Found";
    case 409: return "409 Conflict";
    case 413: return "413 Payload Too Large";
    case 415: return "415 Unsupported Media Type";
    case 429: return "429 Too Many Requests";
    case 500: return "500 Internal Server Error";
    case 503: return "503 Service Unavailable";
    default: return "500 Internal Server Error";
  }
}

void setSecurityHeaders(httpd_req_t* req) {
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

esp_err_t sendBody(httpd_req_t* req, int status, const char* content_type,
                   const std::string& body) {
  setSecurityHeaders(req);
  httpd_resp_set_type(req, content_type);
  httpd_resp_set_status(req, statusText(status));
  httpd_resp_send(req, body.c_str(), body.size());
  return ESP_OK;
}

// Build-scoped validator for the zero-data static pages: the first 8 bytes of
// the application ELF SHA-256, so every firmware build gets a new ETag.
const char* staticEtag() {
  static char etag[24] = "";
  if (etag[0] == '\0') {
    const esp_app_desc_t* app = esp_app_get_description();
    char* out = etag;
    *out++ = '"';
    for (int i = 0; i < 8; ++i) {
      snprintf(out, 3, "%02x", app->app_elf_sha256[i]);
      out += 2;
    }
    *out++ = '"';
    *out = '\0';
  }
  return etag;
}

// Static loader assets live in flash (.rodata); send them straight from there. Browsers revalidate with
// If-None-Match and get a body-less 304 while the firmware build is unchanged.
// csp is set only for HTML documents (nullptr for scripts).
esp_err_t sendStatic(httpd_req_t* req, const char* content_type, const char* body,
                     const char* csp) {
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  httpd_resp_set_hdr(req, "ETag", staticEtag());
  if (csp) httpd_resp_set_hdr(req, "Content-Security-Policy", csp);
  char presented[40];
  if (httpd_req_get_hdr_value_str(req, "If-None-Match", presented, sizeof(presented)) == ESP_OK &&
      strcmp(presented, staticEtag()) == 0) {
    httpd_resp_set_status(req, "304 Not Modified");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
  }
  httpd_resp_set_type(req, content_type);
  httpd_resp_set_status(req, statusText(200));
  httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

esp_err_t sendJson(httpd_req_t* req, int status, const std::string& body) {
  return sendBody(req, status, "application/json; charset=utf-8", body);
}

// 统一错误信封（M13 定稿）。retry_after_s > 0 时带 Retry-After 头。
esp_err_t sendError(httpd_req_t* req, int status, const char* code, const std::string& message,
                    const FieldErrors* fields = nullptr, uint32_t retry_after_s = 0) {
  using alert_core::JsonValue;
  JsonValue::Object err;
  err.emplace_back("code", JsonValue::makeString(code));
  err.emplace_back("message", JsonValue::makeString(message));
  if (fields != nullptr && !fields->empty()) {
    JsonValue::Object fobj;
    for (const auto& f : *fields) fobj.emplace_back(f.field, JsonValue::makeString(f.message));
    err.emplace_back("fields", JsonValue::makeObject(std::move(fobj)));
  }
  const std::string body =
      JsonValue::makeObject({{"error", JsonValue::makeObject(std::move(err))}}).toString();
  if (retry_after_s > 0) {
    char ra[16];
    snprintf(ra, sizeof(ra), "%lu", static_cast<unsigned long>(retry_after_s));
    httpd_resp_set_hdr(req, "Retry-After", ra);
  }
  if (status == 401) {
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer");
  }
  return sendJson(req, status, body);
}

// ---------------------------------------------------------------------------
// 请求工具
// ---------------------------------------------------------------------------
std::string clientIp(httpd_req_t* req) {
  char buf[48] = "0.0.0.0";
  const int fd = httpd_req_to_sockfd(req);
  if (fd >= 0) {
    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    if (getpeername(fd, reinterpret_cast<struct sockaddr*>(&ss), &sl) == 0) {
      if (ss.ss_family == AF_INET) {
        inet_ntop(AF_INET, &reinterpret_cast<struct sockaddr_in*>(&ss)->sin_addr, buf,
                  sizeof(buf));
      } else if (ss.ss_family == AF_INET6) {
        inet_ntop(AF_INET6, &reinterpret_cast<struct sockaddr_in6*>(&ss)->sin6_addr, buf,
                  sizeof(buf));
      }
    }
  }
  return buf;
}

bool headerValue(httpd_req_t* req, const char* name, std::string& out) {
  char buf[256];
  if (httpd_req_get_hdr_value_str(req, name, buf, sizeof(buf)) != ESP_OK) return false;
  out = buf;
  return true;
}

// Host/Origin 白名单（M13 T2：抗 DNS rebinding）。本机 IP 集合：
// STA 当前 IP、AP 固定 192.168.4.1、localhost、配置的主机名（+.local）。
void collectLocalHosts(std::vector<std::string>& out) {
  esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (sta != nullptr) {
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(sta, &info) == ESP_OK && info.ip.addr != 0) {
      char ip[16];
      snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
      out.emplace_back(ip);
    }
  }
  out.emplace_back("192.168.4.1");
  out.emplace_back("127.0.0.1");
  out.emplace_back("localhost");
  if (!g_cfg.hostname.empty()) {
    out.push_back(g_cfg.hostname);
    out.push_back(g_cfg.hostname + ".local");
  }
}

// 去掉 host 头的端口后缀并转小写比较。
bool hostAllowed(const std::string& host_port) {
  std::string host = host_port;
  const size_t colon = host.find(':');
  if (colon != std::string::npos) host = host.substr(0, colon);
  for (auto& c : host) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  std::vector<std::string> locals;
  collectLocalHosts(locals);
  for (const auto& l : locals) {
    if (host == l) return true;
  }
  return false;
}

bool hostOriginOk(httpd_req_t* req) {
  std::string host;
  if (!headerValue(req, "Host", host) || host.empty()) return false;
  if (!hostAllowed(host)) return false;
  std::string origin;
  if (headerValue(req, "Origin", origin) && !origin.empty()) {
    if (origin == "null") return false;
    // scheme://host[:port]
    const size_t scheme = origin.find("://");
    if (scheme == std::string::npos) return false;
    std::string rest = origin.substr(scheme + 3);
    const size_t slash = rest.find('/');
    if (slash != std::string::npos) rest = rest.substr(0, slash);
    if (!hostAllowed(rest)) return false;
  }
  return true;
}

// 读取请求体（预算内）。返回 0=ok；-1=已回 413；-2=已回 400。
int readBodyBudget(httpd_req_t* req, size_t budget, std::string& out) {
  char cl_buf[24];
  if (httpd_req_get_hdr_value_str(req, "Content-Length", cl_buf, sizeof(cl_buf)) == ESP_OK) {
    const long cl = strtol(cl_buf, nullptr, 10);
    if (cl > static_cast<long>(budget)) {
      sendError(req, 413, errc::kPayloadTooLarge, "request body too large");
      return -1;
    }
  }
  out.clear();
  out.reserve(budget < 4096 ? budget : 4096);
  char buf[1024];
  int r;
  while ((r = httpd_req_recv(req, buf, sizeof(buf))) > 0) {
    out.append(buf, static_cast<size_t>(r));
    if (out.size() > budget) {
      sendError(req, 413, errc::kPayloadTooLarge, "request body too large");
      return -1;
    }
  }
  if (r < 0) {
    sendError(req, 400, errc::kInvalidRequest, "incomplete request body");
    return -2;
  }
  return 0;
}

// POST/PUT/DELETE 强制 JSON Content-Type（M13：触发 preflight 即拒 → 415）。
// 前缀必须是 "application/json" 且后随结束/分号/空白——拒绝 "application/jsonx"。
bool contentTypeJsonOk(httpd_req_t* req) {
  std::string ct;
  if (!headerValue(req, "Content-Type", ct)) return false;
  if (ct.compare(0, 16, "application/json") != 0) return false;
  return ct.size() == 16 || ct[16] == ';' || ct[16] == ' ' || ct[16] == '\t';
}

// 解析 JSON body；失败回 400 invalid_request。
bool parseJsonBody(httpd_req_t* req, const std::string& body, alert_core::JsonValue& out) {
  alert_core::JsonParseError perr;
  if (!alert_core::jsonParse(body, out, perr)) {
    sendError(req, 400, errc::kInvalidRequest,
              std::string("invalid JSON at offset ") + std::to_string(perr.offset) + ": " +
                  perr.message);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// 认证（401 同形不区分原因；X-Ingest-Key 越权 → 403）
// ---------------------------------------------------------------------------
bool bearerToken(httpd_req_t* req, std::string& token) {
  std::string auth;
  if (!headerValue(req, "Authorization", auth)) return false;
  if (auth.compare(0, 7, "Bearer ") != 0) return false;
  token = auth.substr(7);
  return !token.empty();
}

bool ingestKeyPresented(httpd_req_t* req) {
  std::string key;
  return headerValue(req, "X-Ingest-Key", key) && !key.empty();
}

std::atomic<int64_t> g_last_authenticated_ms{0};

// Bearer 端点统一认证。失败时已回 401/403。
bool requireBearer(httpd_req_t* req, std::string& token_out) {
  if (ingestKeyPresented(req)) {
    sendError(req, 403, errc::kForbidden, "ingest key cannot access this resource");
    return false;
  }
  std::string token;
  if (!bearerToken(req, token) || !AuthManager::instance().verifyToken(token)) {
    sendError(req, 401, errc::kUnauthorized, "authentication required");
    return false;
  }
  token_out = std::move(token);
  g_last_authenticated_ms.store(esp_timer_get_time() / 1000, std::memory_order_relaxed);
  return true;
}

// 告警摄入端点（M13 T10）：仅 X-Ingest-Key；Bearer 出现 → 403/401。
bool requireIngestKey(httpd_req_t* req) {
  std::string key;
  if (!headerValue(req, "X-Ingest-Key", key) || key.empty()) {
    std::string tok;
    if (bearerToken(req, tok)) {
      if (AuthManager::instance().verifyToken(tok)) {
        sendError(req, 403, errc::kForbidden, "bearer token cannot access this resource");
      } else {
        sendError(req, 401, errc::kUnauthorized, "authentication required");
      }
    } else {
      sendError(req, 401, errc::kUnauthorized, "authentication required");
    }
    return false;
  }
  if (!AuthManager::instance().verifyIngestKey(key)) {
    sendError(req, 401, errc::kUnauthorized, "authentication required");
    return false;
  }
  return true;
}

// 固定窗口限速统一出口（429 + Retry-After）。
bool rateLimit(httpd_req_t* req, const std::string& key, uint32_t window_s, uint32_t limit) {
  uint32_t retry = 0;
  if (AuthManager::instance().consumeBudget(key, window_s, limit, retry)) return true;
  ADMIN_LOG_WARN("rate limited: key=%s", key.c_str());
  sendError(req, 429, errc::kTooManyRequests, "rate limit exceeded", nullptr, retry);
  return false;
}

// JsonValue 只提供 const 访问器；值语义类型，const_cast 安全。
alert_core::JsonValue::Object& mutableItems(alert_core::JsonValue& obj) {
  return const_cast<alert_core::JsonValue::Object&>(obj.objectItems());
}

// 便捷字段错误构造
FieldErrors fieldErr(const std::string& field, const std::string& msg) {
  return FieldErrors{{field, msg}};
}

// 从 JSON body 取字符串字段（缺失/类型错 → false）。
bool jsonStringField(const alert_core::JsonValue& obj, const char* key, std::string& out) {
  const alert_core::JsonValue* v = obj.find(key);
  if (v == nullptr || v->type() != alert_core::JsonValue::Type::String) return false;
  out = v->stringValue();
  return true;
}

// 入队 AdminCommand 统一出口（503/500 映射）。
bool runCommand(httpd_req_t* req, const AdminCommand& cmd, AdminResult& res) {
  if (!enqueueAdminCommand(cmd, res, 10000)) {
    sendError(req, 503, errc::kUnavailable, "command queue unavailable");
    return false;
  }
  if (!res.ok) {
    sendError(req, res.conflict ? 409 : 500,
              res.conflict ? errc::kConflict : errc::kInternal,
              res.err.empty() ? "command failed" : res.err);
    return false;
  }
  return true;
}

}  // namespace

int64_t lastAuthenticatedRequestMs() {
  return g_last_authenticated_ms.load(std::memory_order_relaxed);
}

}  // namespace eego::admin

namespace eego::admin {

namespace {

// ---------------------------------------------------------------------------
// 认证端点
// ---------------------------------------------------------------------------
esp_err_t h_login(httpd_req_t* req) {
  const std::string ip = clientIp(req);
  uint32_t retry = 0;
  if (!AuthManager::instance().loginAllow(ip, retry)) {
    ADMIN_LOG_WARN("login rate limited: ip=%s", ip.c_str());
    return sendError(req, 429, errc::kTooManyRequests, "too many login attempts", nullptr,
                     retry);
  }
  if (!contentTypeJsonOk(req)) {
    return sendError(req, 415, errc::kUnsupportedMediaType, "JSON content type required");
  }
  std::string body;
  const int rb = readBodyBudget(req, kBodyBudgetLogin, body);
  if (rb != 0) return ESP_OK;

  alert_core::JsonValue root;
  if (!parseJsonBody(req, body, root)) return ESP_OK;
  std::string password;
  if (root.type() != alert_core::JsonValue::Type::Object ||
      !jsonStringField(root, "password", password)) {
    return sendError(req, 400, errc::kInvalidRequest, "password (string) required");
  }

  // 未配网（无管理密码）→ 与错密码同形 401，不泄露设备状态（M13 T3）
  if (!AuthManager::instance().hasPassword() ||
      !AuthManager::instance().verifyPassword(password)) {
    AuthManager::instance().noteLoginFailure(ip);
    ADMIN_LOG_WARN("login failed: ip=%s", ip.c_str());
    return sendError(req, 401, errc::kUnauthorized, "authentication required");
  }
  AuthManager::instance().noteLoginSuccess(ip);
  const std::string token = AuthManager::instance().createSession();
  if (token.empty()) {
    return sendError(req, 500, errc::kInternal, "session creation failed");
  }
  ADMIN_LOG_INFO("login ok: ip=%s", ip.c_str());
  using alert_core::JsonValue;
  return sendJson(req, 200,
                  JsonValue::makeObject({
                                            {"token", JsonValue::makeString(token)},
                                            {"token_type", JsonValue::makeString("Bearer")},
                                            {"expires_in",
                                             JsonValue::makeInt(static_cast<int64_t>(
                                                 AuthManager::instance().sessionTtl()))},
                                        })
                      .toString());
}

esp_err_t h_logout(httpd_req_t* req) {
  std::string token;
  if (!requireBearer(req, token)) return ESP_OK;
  AuthManager::instance().revokeSession(token);
  ADMIN_LOG_INFO("logout");
  httpd_resp_set_status(req, statusText(204));
  setSecurityHeaders(req);
  httpd_resp_send(req, "", 0);
  return ESP_OK;
}

esp_err_t h_change_password(httpd_req_t* req) {
  std::string token;
  if (!requireBearer(req, token)) return ESP_OK;
  if (!contentTypeJsonOk(req)) {
    return sendError(req, 415, errc::kUnsupportedMediaType, "JSON content type required");
  }
  std::string body;
  const int rb = readBodyBudget(req, kBodyBudgetLogin, body);
  if (rb != 0) return ESP_OK;
  alert_core::JsonValue root;
  if (!parseJsonBody(req, body, root)) return ESP_OK;
  std::string old_pw, new_pw;
  if (root.type() != alert_core::JsonValue::Type::Object ||
      !jsonStringField(root, "old_password", old_pw) ||
      !jsonStringField(root, "new_password", new_pw)) {
    return sendError(req, 400, errc::kInvalidRequest,
                     "old_password and new_password (string) required");
  }
  // 旧密码验证与登录同桶限速（防持令牌者在线爆破管理密码，review P2-1）
  {
    const std::string ip = clientIp(req);
    uint32_t retry = 0;
    if (!AuthManager::instance().loginAllow(ip, retry)) {
      return sendError(req, 429, errc::kTooManyRequests, "too many attempts", nullptr,
                       retry);
    }
  }
  if (!AuthManager::instance().verifyPassword(old_pw)) {
    AuthManager::instance().noteLoginFailure(clientIp(req));
    return sendError(req, 401, errc::kUnauthorized, "authentication required");
  }
  AuthManager::instance().noteLoginSuccess(clientIp(req));
  if (!AuthManager::passwordPolicyOk(new_pw)) {
    FieldErrors errs =
        fieldErr("new_password", "must be 8..128 chars, not all whitespace");
    return sendError(req, 400, errc::kValidationFailed, "password policy check failed",
                     &errs);
  }
  if (AuthManager::instance().setPassword(new_pw) != ESP_OK) {
    return sendError(req, 500, errc::kInternal, "password update failed");
  }
  ADMIN_LOG_INFO("password changed; all sessions revoked");
  httpd_resp_set_status(req, statusText(204));
  setSecurityHeaders(req);
  httpd_resp_send(req, "", 0);
  return ESP_OK;
}

// ---------------------------------------------------------------------------
// 告警摄入（X-Ingest-Key，M13 T10；httpPushIngest → net_task）
// ---------------------------------------------------------------------------
esp_err_t h_alerts_ingest(httpd_req_t* req) {
  if (!requireIngestKey(req)) return ESP_OK;
  if (!rateLimit(req, "alerts", 60, 30)) return ESP_OK;  // 每 key 30 次/分钟
  if (!contentTypeJsonOk(req)) {
    return sendError(req, 415, errc::kUnsupportedMediaType, "JSON content type required");
  }
  const std::string ip = clientIp(req);
  std::string body;
  const int rb = readBodyBudget(req, kBodyBudgetAlerts, body);
  if (rb != 0) return ESP_OK;

  const auto r = net::httpPushIngest(body, ip);
  using alert_core::JsonValue;
  switch (r.code) {
    case net::NetStack::PushResult::Code::Ok: {
      ADMIN_LOG_INFO("alert ingest: from=%s created=%d updated=%d", ip.c_str(), r.created,
                     r.updated);
      const std::string resp =
          JsonValue::makeObject({{"created", JsonValue::makeInt(r.created)},
                                 {"updated", JsonValue::makeInt(r.updated)}})
              .toString();
      return sendJson(req, r.created > 0 ? 201 : 202, resp);
    }
    case net::NetStack::PushResult::Code::BadRequest: {
      FieldErrors errs{{r.error.field.empty() ? "" : r.error.field, r.error.message}};
      ADMIN_LOG_INFO("alert ingest rejected: from=%s field=%s", ip.c_str(),
                     r.error.field.c_str());
      return sendError(req, 400, errc::kValidationFailed, "alert validation failed", &errs);
    }
    case net::NetStack::PushResult::Code::Oversize:
      return sendError(req, 413, errc::kPayloadTooLarge, "alert batch too large");
    case net::NetStack::PushResult::Code::StoreFull:
      return sendError(req, 503, errc::kStoreFull, "alert store full");
    case net::NetStack::PushResult::Code::Unavailable:
      return sendError(req, 503, errc::kUnavailable, "ingest queue unavailable");
  }
  return sendError(req, 500, errc::kInternal, "ingest failed");
}

// ---------------------------------------------------------------------------
// 告警读取/动作（快照源 + 动作 sink；未接线 → 503）
// ---------------------------------------------------------------------------
std::shared_ptr<const alert_core::AlertSnapshot> acquireSnapshot(bool& wired) {
  const auto& src = internal::snapshotSource();
  wired = static_cast<bool>(src);
  return wired ? src() : nullptr;
}

esp_err_t h_alerts_list(httpd_req_t* req) {
  bool wired = false;
  auto snap = acquireSnapshot(wired);
  if (!wired) {
    ADMIN_LOG_WARN("alerts list: snapshot source not wired");
    return sendError(req, 503, errc::kUnavailable, "alert snapshot source not wired");
  }
  using alert_core::JsonValue;
  // 查询参数（容忍缺省/非法值 → 默认）。M11 契约：state/severity/type/source/
  // muted/since/limit。muted 默认排除（与总览/徽标/默认列表一致），
  // muted=true 才包含静音记录（可查，不丢数据）。
  char q[128] = {};
  std::string state, severity, type, source;
  bool include_muted = false;
  uint64_t since = 0;
  size_t limit = 50;
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
    char v[64];
    if (httpd_query_key_value(q, "state", v, sizeof(v)) == ESP_OK) state = v;
    if (httpd_query_key_value(q, "severity", v, sizeof(v)) == ESP_OK) severity = v;
    if (httpd_query_key_value(q, "type", v, sizeof(v)) == ESP_OK) type = v;
    if (httpd_query_key_value(q, "source", v, sizeof(v)) == ESP_OK) source = v;
    if (httpd_query_key_value(q, "muted", v, sizeof(v)) == ESP_OK) {
      include_muted = (strcmp(v, "true") == 0 || strcmp(v, "1") == 0);
    }
    if (httpd_query_key_value(q, "since", v, sizeof(v)) == ESP_OK) {
      const long long n = strtoll(v, nullptr, 10);
      if (n > 0) since = static_cast<uint64_t>(n);
    }
    if (httpd_query_key_value(q, "limit", v, sizeof(v)) == ESP_OK) {
      const long n = strtol(v, nullptr, 10);
      if (n > 0) limit = static_cast<size_t>(n) > 200 ? 200 : static_cast<size_t>(n);
    }
  }
  JsonValue::Array arr;
  size_t total = 0;
  if (snap != nullptr) {
    for (const auto& rec : snap->records) {
      const std::string st = alert_core::alertStateName(rec.state);
      const std::string sev = alert_core::severityName(rec.severity);
      if (!include_muted && rec.muted) continue;
      if (!state.empty() && st != state) continue;
      if (!severity.empty() && sev != severity) continue;
      if (!type.empty() && rec.type != type) continue;
      if (!source.empty() && rec.source.id != source) continue;
      if (since != 0 && rec.last_seen < since) continue;
      total++;
      if (arr.size() < limit) arr.push_back(alert_core::recordToJson(rec));
    }
  }
  return sendJson(req, 200,
                  JsonValue::makeObject({
                                            {"alerts", JsonValue::makeArray(std::move(arr))},
                                            {"total", JsonValue::makeInt(static_cast<int64_t>(total))},
                                            {"snapshot_seq",
                                             JsonValue::makeInt(snap ? static_cast<int64_t>(snap->seq) : 0)},
                                        })
                      .toString());
}

// 路径段提取：/alerts/<segment>/... 或 /alerts/<segment>（segment 非空且不含 '/'）。
bool alertIdFromPath(const char* path, std::string& id, const char** rest) {
  // path 以 "alerts/" 开头（已由 dispatch 剥离 /api/v1/ 前缀）
  if (strncmp(path, "alerts/", 7) != 0) return false;
  const char* seg = path + 7;
  const char* slash = strchr(seg, '/');
  const size_t len = slash ? static_cast<size_t>(slash - seg) : strlen(seg);
  if (len == 0) return false;
  id.assign(seg, len);
  *rest = slash ? slash + 1 : "";
  return true;
}

esp_err_t mapActionResult(httpd_req_t* req, const AlertActionResult& r, const char* action,
                          const std::string& id) {
  using alert_core::JsonValue;
  switch (r.code) {
    case AlertActionResultCode::Ok: {
      ADMIN_LOG_INFO("alert %s: id=%s by=api:admin", action, id.c_str());
      return sendJson(req, 200,
                      JsonValue::makeObject({
                                                {"ok", JsonValue::makeBool(true)},
                                                {"affected", JsonValue::makeInt(static_cast<int64_t>(r.affected))},
                                            })
                          .toString());
    }
    case AlertActionResultCode::NotFound:
      return sendError(req, 404, errc::kNotFound, "alert not found");
    case AlertActionResultCode::InvalidState:
      return sendError(req, 409, errc::kConflict,
                       r.detail.empty() ? "invalid alert state" : r.detail);
    case AlertActionResultCode::InvalidAction:
      return sendError(req, 400, errc::kInvalidRequest,
                       r.detail.empty() ? "invalid action" : r.detail);
    case AlertActionResultCode::Unavailable:
    case AlertActionResultCode::NoStore:
      return sendError(req, 503, errc::kUnavailable,
                       r.detail.empty() ? "alert store unavailable" : r.detail);
  }
  return sendError(req, 500, errc::kInternal, "alert action failed");
}

esp_err_t h_alert_ack(httpd_req_t* req, const std::string& id) {
  const auto& sink = internal::actionSink();
  if (!sink) {
    ADMIN_LOG_WARN("alert ack: action sink not wired");
    return sendError(req, 503, errc::kUnavailable, "alert action sink not wired");
  }
  return mapActionResult(req, sink(AlertActionKind::Ack, id, "api:admin"), "ack", id);
}

esp_err_t h_alert_clear(httpd_req_t* req, const std::string& id) {
  const auto& sink = internal::actionSink();
  if (!sink) {
    return sendError(req, 503, errc::kUnavailable, "alert action sink not wired");
  }
  return mapActionResult(req, sink(AlertActionKind::Clear, id, "api:admin"), "clear", id);
}

esp_err_t h_alert_ack_all(httpd_req_t* req) {
  const auto& sink = internal::actionSink();
  if (!sink) {
    return sendError(req, 503, errc::kUnavailable, "alert action sink not wired");
  }
  return mapActionResult(req, sink(AlertActionKind::AckAll, "", "api:admin"), "ack-all", "");
}

esp_err_t h_alert_get(httpd_req_t* req, const std::string& id) {
  bool wired = false;
  auto snap = acquireSnapshot(wired);
  if (!wired) {
    return sendError(req, 503, errc::kUnavailable, "alert snapshot source not wired");
  }
  if (snap != nullptr) {
    for (const auto& rec : snap->records) {
      if (rec.id == id) return sendJson(req, 200, alert_core::recordToJson(rec).toString());
    }
  }
  return sendError(req, 404, errc::kNotFound, "alert not found");
}

// ---------------------------------------------------------------------------
// 状态 / 类型 / 日志
// ---------------------------------------------------------------------------
esp_err_t h_status(httpd_req_t* req) {
  using alert_core::JsonValue;
  JsonValue::Object root;

  // device
  const esp_app_desc_t* app = esp_app_get_description();
  char sha_hex[65] = {};
  if (app != nullptr) {
    for (size_t i = 0; i < 32; i++) {
      snprintf(sha_hex + i * 2, 3, "%02x", app->app_elf_sha256[i]);
    }
  }
  char mode[16] = "offline";
  if (ProvisionPortal::instance().isOpen()) {
    snprintf(mode, sizeof(mode), "init_provision");
  } else if (ProvisionPortal::instance().isApFallbackActive()) {
    snprintf(mode, sizeof(mode), "ap_fallback");
  } else if (net::WifiManager::instance().isConnected()) {
    snprintf(mode, sizeof(mode), "online");
  }
  root.emplace_back("device",
                    JsonValue::makeObject({
                        {"fw_version", JsonValue::makeString(app ? app->version : "unknown")},
                        {"fw_sha256", JsonValue::makeString(sha_hex)},
                        {"uptime_s", JsonValue::makeInt(esp_timer_get_time() / 1000000)},
                        {"mode", JsonValue::makeString(mode)},
                    }));

  // network（net_task 快照优先；未运行回退 WifiManager 直读）
  net::NetStatusSnapshot ns;
  const bool ns_ok = net::NetStack::instance().queryStatus(ns);
  JsonValue::Object netobj;
  if (ns_ok) {
    netobj.emplace_back("ssid", JsonValue::makeString(ns.wifi.ssid));
    netobj.emplace_back("ip", JsonValue::makeString(ns.wifi.ip));
    netobj.emplace_back("rssi", JsonValue::makeInt(ns.wifi.rssi));
    netobj.emplace_back("wifi_state", JsonValue::makeInt(ns.wifi.state));
  } else {
    netobj.emplace_back("ssid", JsonValue::makeString(net::WifiManager::instance().currentSsid()));
    netobj.emplace_back("ip", JsonValue::makeString(net::WifiManager::instance().currentIp()));
    netobj.emplace_back("rssi", JsonValue::makeInt(net::WifiManager::instance().rssi()));
    netobj.emplace_back("wifi_state",
                        JsonValue::makeInt(static_cast<int>(net::WifiManager::instance().state())));
  }
  netobj.emplace_back("ap_active", JsonValue::makeBool(ProvisionPortal::instance().apActive()));
  root.emplace_back("network", JsonValue::makeObject(std::move(netobj)));

  // 统一时间源（M21）：net_task 快照优先；未运行回退系统时间可信度下限。
  // valid=false 时 now 可能为 0 —— 客户端不得把 0 当作有效告警时间。
  JsonValue::Object timeobj;
  if (ns_ok) {
    timeobj.emplace_back("valid", JsonValue::makeBool(ns.time.valid));
    timeobj.emplace_back("now", JsonValue::makeInt(static_cast<int64_t>(ns.time.now)));
    timeobj.emplace_back("sntp_synced", JsonValue::makeBool(ns.time.sntp_synced));
    timeobj.emplace_back("last_sync_at",
                         JsonValue::makeInt(static_cast<int64_t>(ns.time.last_sync_at)));
  } else {
    const int64_t sys = static_cast<int64_t>(time(nullptr));
    timeobj.emplace_back("valid", JsonValue::makeBool(sys >= net::kPlausibleUnixFloor));
    timeobj.emplace_back("now", JsonValue::makeInt(sys));
    timeobj.emplace_back("sntp_synced", JsonValue::makeBool(false));
    timeobj.emplace_back("last_sync_at", JsonValue::makeInt(0));
  }
  root.emplace_back("time", JsonValue::makeObject(std::move(timeobj)));

  // memory / storage
  root.emplace_back("memory",
                    JsonValue::makeObject({
                        {"internal_free",
                         JsonValue::makeInt(static_cast<int64_t>(
                             heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)))},
                        {"psram_free",
                         JsonValue::makeInt(static_cast<int64_t>(
                             heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)))},
                    }));
  root.emplace_back("storage",
                    JsonValue::makeObject({
                        {"sd_ready", JsonValue::makeBool(::eego::SdCard::instance().ready())},
                        {"config_valid", JsonValue::makeBool(ConfigStore::instance().valid())},
                    }));

  // alerts（快照源；未接线 → available:false）
  bool wired = false;
  auto snap = acquireSnapshot(wired);
  JsonValue::Object alobj;
  alobj.emplace_back("available", JsonValue::makeBool(wired && snap != nullptr));
  if (wired && snap != nullptr) {
    const auto& s = snap->summary;
    alobj.emplace_back("critical", JsonValue::makeInt(static_cast<int64_t>(s.critical)));
    alobj.emplace_back("warning", JsonValue::makeInt(static_cast<int64_t>(s.warning)));
    alobj.emplace_back("info", JsonValue::makeInt(static_cast<int64_t>(s.info)));
    alobj.emplace_back("active_total", JsonValue::makeInt(static_cast<int64_t>(s.active_total)));
  }
  root.emplace_back("alerts", JsonValue::makeObject(std::move(alobj)));

  // 会话/掩码信息（管理便利，不含任何秘密值）
  root.emplace_back("security",
                    JsonValue::makeObject({
                        {"sessions",
                         JsonValue::makeInt(static_cast<int64_t>(AuthManager::instance().sessionCount()))},
                        {"ingest_key_masked",
                         JsonValue::makeString(AuthManager::instance().maskedIngestKey())},
                        {"provisioned",
                         JsonValue::makeBool(AuthManager::instance().hasPassword())},
                    }));

  return sendJson(req, 200, JsonValue::makeObject(std::move(root)).toString());
}

esp_err_t h_types_list(httpd_req_t* req) {
  alert_core::TypeRegistry* reg = internal::registry();
  if (reg == nullptr) {
    return sendError(req, 503, errc::kUnavailable, "type registry not wired");
  }
  using alert_core::JsonValue;
  JsonValue::Array arr;
  for (const auto& b : reg->builtins()) {
    const alert_core::AlertTypeDescriptor d = reg->resolve(b.id);
    arr.push_back(JsonValue::makeObject({
        {"id", JsonValue::makeString(d.id)},
        {"name_zh", JsonValue::makeString(d.name_zh)},
        {"name_en", JsonValue::makeString(d.name_en)},
        {"severity_default", JsonValue::makeString(alert_core::severityName(d.severity_default))},
        {"group", JsonValue::makeString(d.group)},
        {"ttl_default_s", JsonValue::makeInt(static_cast<int64_t>(d.ttl_default_s))},
        {"muted_default", JsonValue::makeBool(d.muted_default)},
        {"overridden", JsonValue::makeBool(reg->findOverride(b.id) != nullptr)},
    }));
  }
  return sendJson(req, 200,
                  JsonValue::makeObject({{"types", JsonValue::makeArray(std::move(arr))}})
                      .toString());
}

esp_err_t h_type_put(httpd_req_t* req, const std::string& id) {
  alert_core::TypeRegistry* reg = internal::registry();
  if (reg == nullptr) {
    return sendError(req, 503, errc::kUnavailable, "type registry not wired");
  }
  if (!reg->isKnown(id)) {
    return sendError(req, 404, errc::kNotFound, "unknown alert type");
  }
  if (!contentTypeJsonOk(req)) {
    return sendError(req, 415, errc::kUnsupportedMediaType, "JSON content type required");
  }
  std::string body;
  const int rb = readBodyBudget(req, kBodyBudgetSmall, body);
  if (rb != 0) return ESP_OK;
  alert_core::JsonValue patch;
  if (!parseJsonBody(req, body, patch)) return ESP_OK;
  if (patch.type() != alert_core::JsonValue::Type::Object) {
    return sendError(req, 400, errc::kInvalidRequest, "override must be an object");
  }

  using alert_core::JsonValue;
  // 校验并构造新 override（空对象 = 删除覆盖，恢复内置）
  JsonValue::Object ov;
  for (const auto& kv : patch.objectItems()) {
    if (kv.first == "severity") {
      alert_core::Severity sev;
      if (kv.second.type() != JsonValue::Type::String ||
          !alert_core::severityFromString(kv.second.stringValue(), sev)) {
        FieldErrors errs = fieldErr("severity", "must be one of critical|warning|info");
        return sendError(req, 400, errc::kValidationFailed, "validation failed", &errs);
      }
      ov.emplace_back("severity", JsonValue::makeString(kv.second.stringValue()));
    } else if (kv.first == "ttl_s") {
      if (kv.second.type() != JsonValue::Type::Int || kv.second.intValue() < 1 ||
          kv.second.intValue() > alert_core::kMaxTtlSeconds) {
        FieldErrors errs =
            fieldErr("ttl_s", "must be an integer in [1, 31622400]");
        return sendError(req, 400, errc::kValidationFailed, "validation failed", &errs);
      }
      ov.emplace_back("ttl_s", JsonValue::makeInt(kv.second.intValue()));
    } else if (kv.first == "name_zh" || kv.first == "name_en") {
      if (kv.second.type() != JsonValue::Type::String ||
          kv.second.stringValue().empty() || kv.second.stringValue().size() > 64) {
        FieldErrors errs = fieldErr(kv.first, "must be a non-empty string of at most 64 bytes");
        return sendError(req, 400, errc::kValidationFailed, "validation failed", &errs);
      }
      ov.emplace_back(kv.first, JsonValue::makeString(kv.second.stringValue()));
    } else if (kv.first == "muted") {
      if (kv.second.type() != JsonValue::Type::Bool) {
        FieldErrors errs = fieldErr("muted", "must be a boolean");
        return sendError(req, 400, errc::kValidationFailed, "validation failed", &errs);
      }
      ov.emplace_back("muted", JsonValue::makeBool(kv.second.boolValue()));
    } else {
      FieldErrors errs = fieldErr(kv.first, "unknown field");
      return sendError(req, 400, errc::kValidationFailed, "validation failed", &errs);
    }
  }

  // 读改写 alert_type_overrides → AdminCommand 在 app_task 上下文持久化+应用
  JsonValue dom = ConfigStore::instance().rawDom();
  JsonValue::Object* overrides = nullptr;
  for (auto& kv : mutableItems(dom)) {
    if (kv.first == "alert_type_overrides") overrides = &mutableItems(kv.second);
  }
  JsonValue::Object new_overrides;
  if (overrides != nullptr) new_overrides = *overrides;
  if (ov.empty()) {
    for (auto it = new_overrides.begin(); it != new_overrides.end(); ++it) {
      if (it->first == id) {
        new_overrides.erase(it);
        break;
      }
    }
  } else {
    bool replaced = false;
    for (auto& kv : new_overrides) {
      if (kv.first == id) {
        kv.second = JsonValue::makeObject(std::move(ov));
        replaced = true;
        break;
      }
    }
    if (!replaced) new_overrides.emplace_back(id, JsonValue::makeObject(std::move(ov)));
  }
  bool has_section = false;
  for (auto& kv : mutableItems(dom)) {
    if (kv.first == "alert_type_overrides") {
      kv.second = JsonValue::makeObject(std::move(new_overrides));
      has_section = true;
    }
  }
  if (!has_section) {
    mutableItems(dom).emplace_back("alert_type_overrides",
                                   JsonValue::makeObject(std::move(new_overrides)));
  }

  AdminCommand cmd;
  cmd.type = AdminCommandType::ApplyConfig;
  cmd.config = std::move(dom);
  cmd.changed_sections = AdminCommand::kChangedOverrides;
  AdminResult res;
  if (!runCommand(req, cmd, res)) return ESP_OK;

  const alert_core::AlertTypeDescriptor d = reg->resolve(id);
  ADMIN_LOG_INFO("type override applied: id=%s", id.c_str());
  return sendJson(req, 200,
                  JsonValue::makeObject({
                                            {"id", JsonValue::makeString(d.id)},
                                            {"name_zh", JsonValue::makeString(d.name_zh)},
                                            {"name_en", JsonValue::makeString(d.name_en)},
                                            {"severity_default",
                                             JsonValue::makeString(alert_core::severityName(d.severity_default))},
                                            {"ttl_default_s", JsonValue::makeInt(static_cast<int64_t>(d.ttl_default_s))},
                                            {"muted_default", JsonValue::makeBool(d.muted_default)},
                                        })
                      .toString());
}

esp_err_t h_logs(httpd_req_t* req) {
  size_t limit = 50;
  char q[64] = {};
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
    char v[16];
    if (httpd_query_key_value(q, "limit", v, sizeof(v)) == ESP_OK) {
      const long n = strtol(v, nullptr, 10);
      if (n > 0) limit = static_cast<size_t>(n) > 200 ? 200 : static_cast<size_t>(n);
    }
  }
  using alert_core::JsonValue;
  JsonValue::Array arr;
  for (const auto& e : AdminLog::instance().recent(limit)) {
    arr.push_back(JsonValue::makeObject({
        {"ts", JsonValue::makeInt(static_cast<int64_t>(e.ts))},
        {"unsynced", JsonValue::makeBool(e.unsynced)},
        {"level", JsonValue::makeString(logLevelName(e.level))},
        {"seq", JsonValue::makeInt(static_cast<int64_t>(e.seq))},
        {"msg", JsonValue::makeString(e.msg)},
    }));
  }
  return sendJson(req, 200,
                  JsonValue::makeObject({{"entries", JsonValue::makeArray(std::move(arr))}})
                      .toString());
}

}  // namespace

}  // namespace eego::admin

namespace eego::admin {

namespace {

// ---------------------------------------------------------------------------
// 配置端点
// ---------------------------------------------------------------------------
esp_err_t h_config_get(httpd_req_t* req) {
  return sendJson(req, 200, ConfigStore::instance().maskedDom().toString());
}

esp_err_t h_config_put(httpd_req_t* req) {
  if (!contentTypeJsonOk(req)) {
    return sendError(req, 415, errc::kUnsupportedMediaType, "JSON content type required");
  }
  std::string body;
  const int rb = readBodyBudget(req, kBodyBudgetConfig, body);
  if (rb != 0) return ESP_OK;

  alert_core::JsonValue merged;
  FieldErrors errs;
  if (!ConfigStore::instance().validateAndMerge(body, merged, errs)) {
    return sendError(req, 400, errc::kValidationFailed, "config validation failed", &errs);
  }

  uint32_t mask = 0;
  {
    alert_core::JsonValue patch;
    alert_core::JsonParseError perr;
    if (alert_core::jsonParse(body, patch, perr) &&
        patch.type() == alert_core::JsonValue::Type::Object) {
      if (patch.find("admin") != nullptr) mask |= AdminCommand::kChangedAdmin;
      if (patch.find("alert_type_overrides") != nullptr) {
        mask |= AdminCommand::kChangedOverrides;
      }
      if (patch.find("mute_rules") != nullptr) mask |= AdminCommand::kChangedMuteRules;
    }
  }
  AdminCommand cmd;
  cmd.type = AdminCommandType::ApplyConfig;
  cmd.config = std::move(merged);
  cmd.changed_sections = mask;
  AdminResult res;
  if (!runCommand(req, cmd, res)) return ESP_OK;

  ADMIN_LOG_INFO("config updated");
  return sendJson(req, 200, ConfigStore::instance().maskedDom().toString());
}

esp_err_t h_config_schema(httpd_req_t* req) {
  return sendJson(req, 200, ConfigStore::schemaJson());
}

const char* wakeReasonName() {
  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_UNDEFINED: return "power_on_or_reset";
    case ESP_SLEEP_WAKEUP_EXT0: return "power_button";
    case ESP_SLEEP_WAKEUP_TIMER: return "timer";
    default: return "other";
  }
}

esp_err_t h_settings_get(httpd_req_t* req) {
  using alert_core::JsonValue;
  const settings::Snapshot snap = settings::DeviceSettings::instance().snapshot();
  const settings::Capabilities& cap = snap.capabilities;
  const ble::Status ble_state = ble::BleManager::instance().status();
  uint64_t wake_at = 0;
  uint32_t remaining = 0;
  const int64_t now = static_cast<int64_t>(time(nullptr));
  if (now > 0) {
    settings::scheduledSleep(snap.values, static_cast<uint64_t>(now), wake_at, remaining);
  }
  const JsonValue power = JsonValue::makeObject({
      {"last_wake_reason", JsonValue::makeString(wakeReasonName())},
      {"next_scheduled_wake", JsonValue::makeInt(static_cast<int64_t>(wake_at))},
      {"deep_sleep_http_reachable", JsonValue::makeBool(false)},
      {"deep_sleep_ble_reachable", JsonValue::makeBool(false)},
      {"recovery_ap_active", JsonValue::makeBool(ProvisionPortal::instance().apActive())},
      {"recovery_window_remaining_s",
       JsonValue::makeInt(ProvisionPortal::instance().windowRemainingMs() / 1000)},
      {"auto_sleep_suspended",
       JsonValue::makeBool(ProvisionPortal::instance().apActive() || ble_state.stack_active)},
  });
  const JsonValue capabilities = JsonValue::makeObject({
      {"frontlight", JsonValue::makeBool(cap.frontlight)},
      {"battery", JsonValue::makeBool(cap.battery)},
      {"bluetooth", JsonValue::makeBool(cap.bluetooth)},
      {"ble_provisioning", JsonValue::makeBool(cap.ble_provisioning)},
      {"ble_observer", JsonValue::makeBool(cap.ble_observer)},
      {"display", JsonValue::makeBool(cap.display)},
      {"sd", JsonValue::makeBool(cap.sd)},
      {"rtc", JsonValue::makeBool(cap.rtc)},
  });
  const JsonValue ble_json = JsonValue::makeObject({
      {"compiled", JsonValue::makeBool(ble_state.compiled)},
      {"stack_active", JsonValue::makeBool(ble_state.stack_active)},
      {"role", JsonValue::makeString(ble::roleName(ble_state.role))},
      {"provisioning_state", JsonValue::makeString(ble::provisionStateName(ble_state.provisioning))},
      {"provisioning_remaining_s", JsonValue::makeInt(ble_state.provisioning_remaining_s)},
      {"observer_enabled", JsonValue::makeBool(snap.values.ble_observer_enabled)},
      {"observer_remaining_s", JsonValue::makeInt(ble_state.observer_remaining_s)},
      {"recovery_ap_active", JsonValue::makeBool(ble_state.recovery_ap_active)},
      {"deep_sleep_realtime", JsonValue::makeBool(false)},
      {"dropped_advertisements", JsonValue::makeInt(ble_state.dropped_advertisements)},
      {"duplicate_advertisements", JsonValue::makeInt(ble_state.duplicate_advertisements)},
      {"malformed_advertisements", JsonValue::makeInt(ble_state.malformed_advertisements)},
      {"emitted_events", JsonValue::makeInt(ble_state.emitted_events)},
      {"ingest_errors", JsonValue::makeInt(ble_state.ingest_errors)},
      {"diram_free_bytes", JsonValue::makeInt(ble_state.diram_free_bytes)},
      {"diram_min_free_bytes", JsonValue::makeInt(ble_state.diram_min_free_bytes)},
      {"psram_free_bytes", JsonValue::makeInt(ble_state.psram_free_bytes)},
      {"last_error", JsonValue::makeString(ble_state.last_error)},
  });
  return sendJson(req, 200,
                  JsonValue::makeObject({
                      {"schema_version", JsonValue::makeInt(settings::kSchemaVersion)},
                      {"revision", JsonValue::makeInt(snap.revision)},
                      {"settings", settings::toJson(snap.values)},
                      {"capabilities", capabilities},
                      {"power", power},
                      {"ble", ble_json},
                  })
                      .toString());
}

esp_err_t h_ble_unbind(httpd_req_t* req) {
  if (!contentTypeJsonOk(req)) {
    return sendError(req, 415, errc::kUnsupportedMediaType, "JSON content type required");
  }
  std::string body;
  if (readBodyBudget(req, kBodyBudgetLogin, body) != 0) return ESP_OK;
  alert_core::JsonValue root;
  if (!parseJsonBody(req, body, root)) return ESP_OK;
  if (root.type() != alert_core::JsonValue::Type::Object || !root.objectItems().empty()) {
    return sendError(req, 400, errc::kInvalidRequest, "unbind requires an empty object");
  }
  AdminCommand cmd;
  cmd.type = AdminCommandType::BleClearBonds;
  AdminResult res;
  if (!runCommand(req, cmd, res)) return ESP_OK;
  ADMIN_LOG_WARN("BLE bonds cleared via api");
  return sendJson(req, 200, "{\"status\":\"cleared\"}");
}

esp_err_t h_settings_schema(httpd_req_t* req) {
  return sendJson(req, 200, settings::schemaJson());
}

esp_err_t h_settings_put(httpd_req_t* req) {
  if (!contentTypeJsonOk(req)) {
    return sendError(req, 415, errc::kUnsupportedMediaType, "JSON content type required");
  }
  std::string body;
  const int rb = readBodyBudget(req, kBodyBudgetConfig, body);
  if (rb != 0) return ESP_OK;
  alert_core::JsonValue patch;
  if (!parseJsonBody(req, body, patch)) return ESP_OK;

  const settings::Snapshot snap = settings::DeviceSettings::instance().snapshot();
  settings::Settings candidate;
  settings::FieldErrors settings_errors;
  if (!settings::mergePatch(snap.values, patch, candidate, settings_errors)) {
    FieldErrors errors;
    for (const auto& error : settings_errors) errors.push_back({error.field, error.message});
    return sendError(req, 400, errc::kValidationFailed, "settings validation failed", &errors);
  }
  if ((settings::changedFields(snap.values, candidate) & settings::kChangedFrontlight) != 0 &&
      !snap.capabilities.frontlight) {
    FieldErrors errors = fieldErr("frontlight_enabled", "frontlight hardware is unavailable");
    return sendError(req, 400, errc::kValidationFailed, "settings validation failed", &errors);
  }
  if (candidate.ble_observer_enabled && !snap.capabilities.ble_observer) {
    FieldErrors errors = fieldErr("ble_observer_enabled", "BLE observer is unavailable");
    return sendError(req, 400, errc::kValidationFailed, "settings validation failed", &errors);
  }

  AdminCommand cmd;
  cmd.type = AdminCommandType::ApplySettings;
  cmd.settings_patch = std::move(patch);
  AdminResult res;
  if (!runCommand(req, cmd, res)) return ESP_OK;
  ADMIN_LOG_INFO("typed device settings updated");
  return h_settings_get(req);
}

esp_err_t h_input_bindings_get(httpd_req_t* req) {
  using alert_core::JsonValue;
  const InputBindingSet snap = InputBindings::instance().snapshot();
  JsonValue root = inputBindingsToJson(snap);
  mutableItems(root).emplace_back("defaults", inputBindingListToJson(defaultInputBindings()));
  mutableItems(root).emplace_back("loaded", JsonValue::makeBool(snap.loaded));
  mutableItems(root).emplace_back("recovered", JsonValue::makeBool(snap.recovered));
  return sendJson(req, 200, root.toString());
}

esp_err_t h_input_bindings_schema(httpd_req_t* req) {
  const auto document = DashboardStore::instance().snapshot();
  const dashboard::Dashboard* current =
      document != nullptr && document->configured ? &document->dashboard : nullptr;
  return sendJson(req, 200, inputBindingSchema(current).toString());
}

esp_err_t h_input_bindings_write(httpd_req_t* req, bool reset) {
  using alert_core::JsonValue;
  if (!contentTypeJsonOk(req)) {
    return sendError(req, 415, errc::kUnsupportedMediaType, "JSON content type required");
  }
  std::string body;
  const int rb = readBodyBudget(req, kBodyBudgetSmall, body);
  if (rb != 0) return ESP_OK;
  JsonValue root;
  if (!parseJsonBody(req, body, root)) return ESP_OK;
  if (reset && root.type() == JsonValue::Type::Object) {
    const JsonValue* bindings = root.find("bindings");
    if (bindings == nullptr) {
      mutableItems(root).emplace_back("bindings", JsonValue::makeArray({}));
    } else if (bindings->type() != JsonValue::Type::Array || !bindings->arrayItems().empty()) {
      return sendError(req, 400, errc::kValidationFailed, "reset bindings must be empty");
    }
  }

  InputBindingSet candidate;
  InputBindingErrors binding_errors;
  if (!inputBindingsFromJson(root, true, candidate, binding_errors)) {
    FieldErrors errors;
    for (const auto& error : binding_errors) errors.push_back({error.field, error.message});
    return sendError(req, 400, errc::kValidationFailed, "input binding validation failed", &errors);
  }
  const auto document = DashboardStore::instance().snapshot();
  const dashboard::Dashboard* current =
      document != nullptr && document->configured ? &document->dashboard : nullptr;
  if (!validateInputBindings(reset ? std::vector<InputBinding>{} : candidate.bindings,
                             current, binding_errors)) {
    FieldErrors errors;
    for (const auto& error : binding_errors) errors.push_back({error.field, error.message});
    return sendError(req, 400, errc::kValidationFailed, "input binding validation failed", &errors);
  }

  AdminCommand cmd;
  cmd.type = AdminCommandType::ApplyInputBindings;
  cmd.input_reset = reset;
  cmd.input_bindings = std::make_shared<InputBindingSet>(std::move(candidate));
  AdminResult res;
  if (!runCommand(req, cmd, res)) return ESP_OK;
  ADMIN_LOG_INFO(reset ? "input bindings reset" : "input bindings updated");
  return h_input_bindings_get(req);
}

esp_err_t h_wifi_add(httpd_req_t* req) {
  if (!contentTypeJsonOk(req)) {
    return sendError(req, 415, errc::kUnsupportedMediaType, "JSON content type required");
  }
  std::string body;
  const int rb = readBodyBudget(req, kBodyBudgetSmall, body);
  if (rb != 0) return ESP_OK;
  alert_core::JsonValue root;
  if (!parseJsonBody(req, body, root)) return ESP_OK;
  std::string ssid, password;
  if (root.type() != alert_core::JsonValue::Type::Object ||
      !jsonStringField(root, "ssid", ssid)) {
    return sendError(req, 400, errc::kInvalidRequest, "ssid (string) required");
  }
  jsonStringField(root, "password", password);  // 开放网络可空
  FieldErrors errs;
  if (ssid.empty() || ssid.size() > 32) {
    errs.push_back({"ssid", "must be 1..32 bytes"});
  }
  if (!password.empty() && (password.size() < 8 || password.size() > 63)) {
    errs.push_back({"password", "must be empty (open) or 8..63 bytes"});
  }
  if (!errs.empty()) {
    return sendError(req, 400, errc::kValidationFailed, "validation failed", &errs);
  }

  AdminCommand cmd;
  cmd.type = AdminCommandType::ApplyWifi;
  cmd.wifi_ssid = ssid;
  cmd.wifi_password = password;
  cmd.wifi_remove = false;
  AdminResult res;
  if (!runCommand(req, cmd, res)) return ESP_OK;

  ADMIN_LOG_INFO("wifi account saved via api: ssid=%s", ssid.c_str());
  // 201 返回最新 wifi_list（掩码 DOM 中的 admin.wifi_list）
  alert_core::JsonValue dom = ConfigStore::instance().maskedDom();
  const alert_core::JsonValue* wl = nullptr;
  if (const alert_core::JsonValue* admin = dom.find("admin")) wl = admin->find("wifi_list");
  return sendJson(req, 201, (wl != nullptr) ? wl->toString() : "[]");
}

esp_err_t h_wifi_remove(httpd_req_t* req) {
  if (!contentTypeJsonOk(req)) {
    return sendError(req, 415, errc::kUnsupportedMediaType, "JSON content type required");
  }
  std::string body;
  const int rb = readBodyBudget(req, kBodyBudgetSmall, body);
  if (rb != 0) return ESP_OK;
  alert_core::JsonValue root;
  if (!parseJsonBody(req, body, root)) return ESP_OK;
  std::string ssid;
  if (root.type() != alert_core::JsonValue::Type::Object ||
      !jsonStringField(root, "ssid", ssid) || ssid.empty()) {
    return sendError(req, 400, errc::kInvalidRequest, "ssid (string) required");
  }
  AdminCommand cmd;
  cmd.type = AdminCommandType::ApplyWifi;
  cmd.wifi_ssid = ssid;
  cmd.wifi_remove = true;
  AdminResult res;
  if (!runCommand(req, cmd, res)) return ESP_OK;
  ADMIN_LOG_INFO("wifi account removed via api: ssid=%s", ssid.c_str());
  httpd_resp_set_status(req, statusText(204));
  setSecurityHeaders(req);
  httpd_resp_send(req, "", 0);
  return ESP_OK;
}

// ---------------------------------------------------------------------------
// sources/credentials（write-only；固定 source 命名空间，不返回秘密值）
// ---------------------------------------------------------------------------
esp_err_t h_source_credentials_get(httpd_req_t* req) {
  std::vector<net::SourceCredentialMetadata> credentials;
  if (net::CredentialStore::listSourceCredentials(credentials) != ESP_OK) {
    return sendError(req, 503, errc::kUnavailable, "source credential store unavailable");
  }
  using alert_core::JsonValue;
  JsonValue::Array items;
  for (const auto& credential : credentials) {
    items.push_back(JsonValue::makeObject({
        {"key", JsonValue::makeString(credential.key)},
        {"ref", JsonValue::makeString(credential.ref)},
    }));
  }
  return sendJson(req, 200,
                  JsonValue::makeObject({
                      {"namespace", JsonValue::makeString("source")},
                      {"credentials", JsonValue::makeArray(std::move(items))},
                  }).toString());
}

esp_err_t h_source_credentials_write(httpd_req_t* req, bool remove) {
  struct SecretGuard {
    std::string value;
    ~SecretGuard() { net::CredentialStore::secureErase(value); }
  } secret;
  if (!contentTypeJsonOk(req)) {
    return sendError(req, 415, errc::kUnsupportedMediaType, "JSON content type required");
  }
  std::string body;
  if (readBodyBudget(req, kBodyBudgetSmall, body) != 0) return ESP_OK;
  alert_core::JsonValue root;
  if (!parseJsonBody(req, body, root)) {
    net::CredentialStore::secureErase(body);
    return ESP_OK;
  }
  net::CredentialStore::secureErase(body);
  using alert_core::JsonValue;
  std::string key;
  bool shape = root.type() == JsonValue::Type::Object && jsonStringField(root, "key", key);
  if (!remove) shape = shape && jsonStringField(root, "value", secret.value);
  if (shape) {
    for (const auto& member : root.objectItems()) {
      if (member.first != "key" && (remove || member.first != "value")) shape = false;
    }
  }
  root = JsonValue::makeNull();
  if (!shape) {
    return sendError(req, 400, errc::kInvalidRequest,
                     remove ? "credential key is required" : "credential key and value are required");
  }
  FieldErrors errs;
  if (!net::CredentialStore::validSourceKeyForWrite(key)) {
    errs.push_back({"key", "must be 1..15 [A-Za-z0-9_-] chars and not a reserved device key"});
  }
  if (!remove && !net::CredentialStore::validSourceValue(secret.value)) {
    errs.push_back({"value", "must be 1..1024 bytes without control characters"});
  }
  if (!errs.empty()) {
    return sendError(req, 400, errc::kValidationFailed, "validation failed", &errs);
  }

  AdminCommand cmd;
  cmd.type = remove ? AdminCommandType::DeleteSourceCredential
                    : AdminCommandType::WriteSourceCredential;
  cmd.credential_key = key;
  cmd.credential_value = secret.value;
  AdminResult res;
  const bool ok = runCommand(req, cmd, res);
  net::CredentialStore::secureErase(cmd.credential_value);
  if (!ok) return ESP_OK;
  ADMIN_LOG_INFO(remove ? "source credential deleted" : "source credential written");
  return sendJson(req, 200,
                  JsonValue::makeObject({
                      {"key", JsonValue::makeString(key)},
                      {"ref", JsonValue::makeString(net::CredentialStore::sourceRef(key))},
                      {"deleted", JsonValue::makeBool(remove)},
                  }).toString());
}

// ---------------------------------------------------------------------------
// 系统端点（reboot / factory-reset 双确认守卫；ingest-key 轮换仅显示一次）
// ---------------------------------------------------------------------------
esp_err_t h_reboot(httpd_req_t* req) {
  if (!contentTypeJsonOk(req)) {
    return sendError(req, 415, errc::kUnsupportedMediaType, "JSON content type required");
  }
  std::string body;
  const int rb = readBodyBudget(req, kBodyBudgetConfirm, body);
  if (rb != 0) return ESP_OK;
  alert_core::JsonValue root;
  if (!parseJsonBody(req, body, root)) return ESP_OK;
  std::string confirm;
  if (!jsonStringField(root, "confirm", confirm) || confirm != "REBOOT") {
    FieldErrors errs = fieldErr("confirm", "must be \"REBOOT\"");
    return sendError(req, 400, errc::kValidationFailed, "validation failed", &errs);
  }
  AdminCommand cmd;
  cmd.type = AdminCommandType::Reboot;
  cmd.delay_ms = 2000;  // M13：202 后 ≥2s 重启
  AdminResult res;
  if (!runCommand(req, cmd, res)) return ESP_OK;
  ADMIN_LOG_WARN("reboot scheduled via api");
  return sendJson(req, 202, "{\"status\":\"reboot scheduled\"}");
}

esp_err_t h_factory_reset(httpd_req_t* req) {
  if (!contentTypeJsonOk(req)) {
    return sendError(req, 415, errc::kUnsupportedMediaType, "JSON content type required");
  }
  std::string body;
  const int rb = readBodyBudget(req, kBodyBudgetSmall, body);
  if (rb != 0) return ESP_OK;
  alert_core::JsonValue root;
  if (!parseJsonBody(req, body, root)) return ESP_OK;
  std::string password, confirm;
  if (!jsonStringField(root, "password", password) ||
      !jsonStringField(root, "confirm", confirm)) {
    return sendError(req, 400, errc::kInvalidRequest,
                     "password and confirm (string) required");
  }
  if (confirm != "RESET") {
    FieldErrors errs = fieldErr("confirm", "must be \"RESET\"");
    return sendError(req, 400, errc::kValidationFailed, "validation failed", &errs);
  }
  // 双确认：会话之外再验一次管理密码（防会话劫持后被远程 wipe，M13 F8）；
  // 与登录同桶限速（review P2-1）
  {
    const std::string ip = clientIp(req);
    uint32_t retry = 0;
    if (!AuthManager::instance().loginAllow(ip, retry)) {
      return sendError(req, 429, errc::kTooManyRequests, "too many attempts", nullptr,
                       retry);
    }
  }
  if (!AuthManager::instance().verifyPassword(password)) {
    AuthManager::instance().noteLoginFailure(clientIp(req));
    return sendError(req, 401, errc::kUnauthorized, "authentication required");
  }
  AuthManager::instance().noteLoginSuccess(clientIp(req));
  AdminCommand cmd;
  cmd.type = AdminCommandType::FactoryReset;
  cmd.delay_ms = 2000;
  AdminResult res;
  if (!runCommand(req, cmd, res)) return ESP_OK;
  ADMIN_LOG_WARN("factory reset scheduled via api");
  return sendJson(req, 202, "{\"status\":\"factory reset scheduled\"}");
}

esp_err_t h_ingest_rotate(httpd_req_t* req) {
  const std::string key = AuthManager::instance().rotateIngestKey();
  if (key.empty()) {
    return sendError(req, 500, errc::kInternal, "ingest key rotation failed");
  }
  ADMIN_LOG_WARN("ingest key rotated; previous key revoked");
  // M13：仅本次完整返回；之后任何响应只含前 4 位掩码
  return sendJson(req, 200,
                  alert_core::JsonValue::makeObject({
                                            {"ingest_key", alert_core::JsonValue::makeString(key)},
                                        })
                      .toString());
}

// ---------------------------------------------------------------------------
// GET / and /loader.js: zero-data loader for the externally hosted React app.
// The page CSP allows scripts/styles from EEGO_WEB_APP_ORIGIN only.
// ---------------------------------------------------------------------------
esp_err_t h_root_page(httpd_req_t* req) {
  return sendStatic(req, "text/html; charset=utf-8", pages::kLoaderHtml, pages::kLoaderCsp);
}
esp_err_t h_loader_js(httpd_req_t* req) {
  return sendStatic(req, "application/javascript; charset=utf-8", pages::kLoaderJs, nullptr);
}

}  // namespace

}  // namespace eego::admin

namespace eego::admin {

namespace {

// ---------------------------------------------------------------------------
// 显示快照（GET /api/v1/display/snapshot.bmp）
// ---------------------------------------------------------------------------
std::mutex g_snapshot_mutex;
DisplaySnapshotProvider g_snapshot_provider;

struct PsramDeleter {
  void operator()(uint8_t* p) const { heap_caps_free(p); }
};

esp_err_t h_display_snapshot(httpd_req_t* req) {
  DisplaySnapshotProvider provider;
  {
    std::lock_guard<std::mutex> lock(g_snapshot_mutex);
    provider = g_snapshot_provider;
  }
  if (!provider) return sendError(req, 503, errc::kUnavailable, "display snapshot unavailable");
  // ~55 KB: keep it out of internal RAM.
  std::unique_ptr<uint8_t, PsramDeleter> bmp(
      static_cast<uint8_t*>(heap_caps_calloc(1, kSnapshotFileBytes, MALLOC_CAP_SPIRAM)));
  if (!bmp) return sendError(req, 503, errc::kUnavailable, "snapshot buffer unavailable");
  writeSnapshotBmpHeader(bmp.get());
  if (!provider(bmp.get() + kSnapshotHeaderBytes, kSnapshotStride)) {
    return sendError(req, 503, errc::kUnavailable, "display frame unavailable");
  }
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_type(req, "image/bmp");
  httpd_resp_set_status(req, statusText(200));
  // Sent in 4 KiB chunks straight from the PSRAM buffer.
  constexpr size_t kChunk = 4096;
  for (size_t offset = 0; offset < kSnapshotFileBytes; offset += kChunk) {
    const size_t n = std::min(kChunk, kSnapshotFileBytes - offset);
    if (httpd_resp_send_chunk(req, reinterpret_cast<const char*>(bmp.get() + offset),
                              static_cast<ssize_t>(n)) != ESP_OK) {
      return ESP_FAIL;
    }
  }
  httpd_resp_send_chunk(req, nullptr, 0);
  return ESP_OK;
}

// ---------------------------------------------------------------------------
// 路由分发（/api/v1/*；M13 定稿路由表；其余一切 → 404 JSON，含 /firmware/*）
// ---------------------------------------------------------------------------
esp_err_t apiDispatch(httpd_req_t* req, const char* path) {
  // Host/Origin 白名单（M13 T2：抗 DNS rebinding；无 CORS 头）
  if (!hostOriginOk(req)) {
    ADMIN_LOG_WARN("host/origin rejected");
    return sendError(req, 403, errc::kForbidden, "forbidden");
  }

  const auto m = req->method;

  // POST /auth/login（公开 + 登录限速）
  if (m == HTTP_POST && strcmp(path, "auth/login") == 0) {
    return h_login(req);
  }

  // POST /alerts（X-Ingest-Key，M13 T10 最小权限）
  if (m == HTTP_POST && strcmp(path, "alerts") == 0) {
    return h_alerts_ingest(req);
  }

  // ---- 以下全部 Bearer ----
  std::string token;
  auto bearer = [&]() { return requireBearer(req, token); };

  // DP5：完整 dashboard 配置中心。沿用 Host/Origin + Bearer；GET、写操作、
  // 出网测试分别限速，禁止 X-Ingest-Key 越权访问。限速分类与 handler 必须
  // 使用同一规范路径，防止重复/尾斜杠变体落入普通写操作桶。
  if (strcmp(path, "dashboard") == 0 || strncmp(path, "dashboard/", 10) == 0) {
    if (!bearer()) return ESP_OK;
    const std::string canonical_path = internal::canonicalDashboardApiPath(path);
    if (m == HTTP_GET) {
      if (!rateLimit(req, "get:" + clientIp(req), 60, 60)) return ESP_OK;
    } else if (m == HTTP_POST && strcmp(canonical_path.c_str(), "dashboard/sources/test") == 0) {
      if (!rateLimit(req, "dashboard-test:" + token.substr(0, 8), 60, 5)) return ESP_OK;
    } else if (m == HTTP_POST && strcmp(canonical_path.c_str(), "dashboard/sources/probe") == 0) {
      // Outbound fetch that echoes a (masked, bounded) body: own small bucket.
      if (!rateLimit(req, "dashboard-probe:" + token.substr(0, 8), 60, 5)) return ESP_OK;
    } else if (m == HTTP_POST && strcmp(canonical_path.c_str(), "dashboard/render-preview") == 0) {
      // Offscreen render on app_task (~400 KB PSRAM, blocks the UI loop briefly).
      if (!rateLimit(req, "dashboard-preview:" + token.substr(0, 8), 60, 10)) return ESP_OK;
    } else if (!rateLimit(req, "dashboard-write:" + token.substr(0, 8), 60, 30)) {
      return ESP_OK;
    }
    return internal::handleDashboardApi(req, canonical_path.c_str());
  }

  if (m == HTTP_GET) {
    if (!rateLimit(req, "get:" + clientIp(req), 60, 60)) return ESP_OK;  // GET 60/min/IP
    if (strcmp(path, "status") == 0) {
      if (!bearer()) return ESP_OK;
      return h_status(req);
    }
    if (strcmp(path, "types") == 0) {
      if (!bearer()) return ESP_OK;
      return h_types_list(req);
    }
    if (strcmp(path, "system/logs") == 0) {
      if (!bearer()) return ESP_OK;
      return h_logs(req);
    }
    if (strcmp(path, "display/snapshot.bmp") == 0) {
      if (!bearer()) return ESP_OK;
      return h_display_snapshot(req);
    }
    if (strcmp(path, "config") == 0) {
      if (!bearer()) return ESP_OK;
      return h_config_get(req);
    }
    if (strcmp(path, "config/schema") == 0) {
      if (!bearer()) return ESP_OK;
      return h_config_schema(req);
    }
    if (strcmp(path, "settings") == 0) {
      if (!bearer()) return ESP_OK;
      return h_settings_get(req);
    }
    if (strcmp(path, "settings/schema") == 0) {
      if (!bearer()) return ESP_OK;
      return h_settings_schema(req);
    }
    if (strcmp(path, "input-bindings") == 0) {
      if (!bearer()) return ESP_OK;
      return h_input_bindings_get(req);
    }
    if (strcmp(path, "input-bindings/schema") == 0) {
      if (!bearer()) return ESP_OK;
      return h_input_bindings_schema(req);
    }
    if (strcmp(path, "sources/credentials") == 0) {
      if (!bearer()) return ESP_OK;
      return h_source_credentials_get(req);
    }
    if (strcmp(path, "alerts") == 0) {
      if (!bearer()) return ESP_OK;
      return h_alerts_list(req);
    }
    // GET /alerts/{id}
    {
      std::string id;
      const char* rest = nullptr;
      if (alertIdFromPath(path, id, &rest) && rest[0] == '\0') {
        if (!bearer()) return ESP_OK;
        return h_alert_get(req, id);
      }
    }
    return sendError(req, 404, errc::kNotFound, "resource not found");
  }

  if (m == HTTP_POST) {
    if (strcmp(path, "auth/logout") == 0) {
      if (!bearer()) return ESP_OK;
      return h_logout(req);
    }
    if (strcmp(path, "auth/change-password") == 0) {
      if (!bearer()) return ESP_OK;
      return h_change_password(req);
    }
    if (strcmp(path, "alerts/ack-all") == 0) {
      if (!bearer()) return ESP_OK;
      return h_alert_ack_all(req);
    }
    // POST /alerts/{id}/ack | /clear
    {
      std::string id;
      const char* rest = nullptr;
      if (alertIdFromPath(path, id, &rest)) {
        if (strcmp(rest, "ack") == 0) {
          if (!bearer()) return ESP_OK;
          return h_alert_ack(req, id);
        }
        if (strcmp(rest, "clear") == 0) {
          if (!bearer()) return ESP_OK;
          return h_alert_clear(req, id);
        }
      }
    }
    if (strcmp(path, "config/wifi") == 0) {
      if (!bearer()) return ESP_OK;
      return h_wifi_add(req);
    }
    if (strcmp(path, "input-bindings/reset") == 0) {
      if (!bearer()) return ESP_OK;
      if (!rateLimit(req, "input-write:" + token.substr(0, 8), 60, 30)) return ESP_OK;
      return h_input_bindings_write(req, true);
    }
    if (strcmp(path, "settings/ble/unbind") == 0) {
      if (!bearer()) return ESP_OK;
      if (!rateLimit(req, "ble-unbind:" + token.substr(0, 8), 60, 5)) return ESP_OK;
      return h_ble_unbind(req);
    }
    if (strcmp(path, "system/reboot") == 0) {
      if (!bearer()) return ESP_OK;
      return h_reboot(req);
    }
    if (strcmp(path, "system/factory-reset") == 0) {
      if (!bearer()) return ESP_OK;
      return h_factory_reset(req);
    }
    if (strcmp(path, "system/ingest-key/rotate") == 0) {
      if (!bearer()) return ESP_OK;
      return h_ingest_rotate(req);
    }
    return sendError(req, 404, errc::kNotFound, "resource not found");
  }

  if (m == HTTP_PUT) {
    if (strcmp(path, "config") == 0) {
      if (!bearer()) return ESP_OK;
      return h_config_put(req);
    }
    if (strcmp(path, "settings") == 0) {
      if (!bearer()) return ESP_OK;
      return h_settings_put(req);
    }
    if (strcmp(path, "sources/credentials") == 0) {
      if (!bearer()) return ESP_OK;
      if (!rateLimit(req, "source-credential-write:" + token.substr(0, 8), 60, 30)) return ESP_OK;
      return h_source_credentials_write(req, false);
    }
    if (strcmp(path, "input-bindings") == 0) {
      if (!bearer()) return ESP_OK;
      if (!rateLimit(req, "input-write:" + token.substr(0, 8), 60, 30)) return ESP_OK;
      return h_input_bindings_write(req, false);
    }
    // PUT /types/{id}
    if (strncmp(path, "types/", 6) == 0 && path[6] != '\0' && strchr(path + 6, '/') == nullptr) {
      if (!bearer()) return ESP_OK;
      return h_type_put(req, path + 6);
    }
    return sendError(req, 404, errc::kNotFound, "resource not found");
  }

  if (m == HTTP_DELETE) {
    if (strcmp(path, "config/wifi") == 0) {
      if (!bearer()) return ESP_OK;
      return h_wifi_remove(req);
    }
    if (strcmp(path, "sources/credentials") == 0) {
      if (!bearer()) return ESP_OK;
      if (!rateLimit(req, "source-credential-write:" + token.substr(0, 8), 60, 30)) return ESP_OK;
      return h_source_credentials_write(req, true);
    }
    return sendError(req, 404, errc::kNotFound, "resource not found");
  }

  return sendError(req, 404, errc::kNotFound, "resource not found");
}

// 通配 handler：静态页（免认证零数据）+ API 命名空间 + 404
esp_err_t wildcardHandler(httpd_req_t* req) {
  const char* uri = req->uri;
  if (req->method == HTTP_GET) {
    if (strcmp(uri, "/") == 0 || strncmp(uri, "/?", 2) == 0) return h_root_page(req);
    if (strcmp(uri, "/loader.js") == 0) return h_loader_js(req);
  }
  if (strncmp(uri, "/api/v1/", 8) == 0) {
    std::string path(uri + 8);
    const size_t query = path.find('?');
    if (query != std::string::npos) path.resize(query);
    return apiDispatch(req, path.c_str());
  }
  // 含 /api/v1/firmware/* 在内的一切未知路径 → 404 JSON（M13 T7：首版无远程 OTA）
  return sendError(req, 404, errc::kNotFound, "resource not found");
}

}  // namespace

// ===========================================================================
// 服务器生命周期（app_task 按设备模式启停；幂等）
// ===========================================================================

esp_err_t startAdminApi(const AdminConfig& cfg) {
  if (g_server != nullptr) return ESP_ERR_INVALID_STATE;
  // 80 端口互斥（防御性检查；模式判定本就在 app_task 上下文）
  if (ProvisionPortal::instance().isOpen()) return ESP_ERR_INVALID_STATE;

  // AdminLog / AuthManager / ConfigStore / DashboardStore are initialized
  // exactly once by the composition root (app_main) before this is called.
  g_cfg = cfg;
  internal::setRegistry(cfg.registry);

  // 应用 admin 域运行参数
  const AdminDomainConfig admin = ConfigStore::instance().admin();
  AuthManager::instance().setSessionTtl(admin.session_ttl_s);
  AuthManager::instance().setLockThreshold(admin.login_lock_threshold);

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = cfg.port;
  config.max_uri_handlers = 12;
  config.stack_size = 8192;       // M12 任务表：httpd 8KB
  config.max_open_sockets = 5;    // lwIP 预算（M13 T6；httpd 内部另占 3）
  config.recv_wait_timeout = 5;   // 慢速 POST 5s 断开（M13 T6）
  config.lru_purge_enable = true;
  config.keep_alive_enable = false;  // 无 keep-alive（M13 传输约定）
  config.uri_match_fn = httpd_uri_match_wildcard;

  esp_err_t err = httpd_start(&g_server, &config);
  if (err != ESP_OK) {
    g_server = nullptr;
    return err;
  }

  const httpd_uri_t routes[] = {
      {.uri = "/*", .method = HTTP_GET, .handler = &wildcardHandler},
      {.uri = "/*", .method = HTTP_POST, .handler = &wildcardHandler},
      {.uri = "/*", .method = HTTP_PUT, .handler = &wildcardHandler},
      {.uri = "/*", .method = HTTP_DELETE, .handler = &wildcardHandler},
  };
  for (const auto& r : routes) {
    httpd_register_uri_handler(g_server, &r);
  }
  ADMIN_LOG_INFO("admin api started on port %u", static_cast<unsigned>(cfg.port));
  return ESP_OK;
}

bool adminApiRunning() { return g_server != nullptr; }

void setDisplaySnapshotProvider(DisplaySnapshotProvider provider) {
  std::lock_guard<std::mutex> lock(g_snapshot_mutex);
  g_snapshot_provider = std::move(provider);
}

}  // namespace eego::admin
