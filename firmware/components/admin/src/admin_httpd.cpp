// SPDX-License-Identifier: GPL-3.0-or-later
//
//

#include "admin/admin.h"
#include "alert_core/json_writer.h"
#include "admin/device_log.h"
#include "admin/display_snapshot.h"
#include "admin/device_screen.h"
#include "admin/keep_awake.h"

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
#include "wifi_diag.h"
#include "device_settings/device_settings.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#include "net_stack/http_push_bridge.h"
#include "net_stack/net_task.h"
#include "net_stack/wifi_list.h"
#include "net_stack/wifi_manager.h"
#include "admin/provision_portal.h"
#include "sd_card.h"

namespace eego::admin {
namespace {
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
httpd_handle_t g_server = nullptr;
AdminConfig g_cfg;

constexpr size_t kBodyBudgetLogin = 1024;
constexpr size_t kBodyBudgetConfig = 4 * 1024;
constexpr size_t kBodyBudgetAlerts = 64 * 1024;
constexpr size_t kBodyBudgetSmall = 8 * 1024;
constexpr size_t kBodyBudgetConfirm = 512;

// ---------------------------------------------------------------------------
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
      ::eego::json::ObjectWriter(1)
          .value("error", ::eego::json::JsonValue(JsonValue::makeObject(std::move(err))))
          .take()
          .toString();
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

bool viaSetupHotspot(httpd_req_t* req) {
  const int fd = httpd_req_to_sockfd(req);
  if (fd < 0) return false;
  struct sockaddr_storage ss;
  socklen_t sl = sizeof(ss);
  if (getsockname(fd, reinterpret_cast<struct sockaddr*>(&ss), &sl) != 0) return false;
  char buf[48] = "";
  if (ss.ss_family == AF_INET) {
    inet_ntop(AF_INET, &reinterpret_cast<struct sockaddr_in*>(&ss)->sin_addr, buf, sizeof(buf));
  } else if (ss.ss_family == AF_INET6) {
    inet_ntop(AF_INET6, &reinterpret_cast<struct sockaddr_in6*>(&ss)->sin6_addr, buf,
              sizeof(buf));
  }
  const size_t n = strlen(buf);
  constexpr char kAp[] = "192.168.4.1";
  constexpr size_t kApLen = sizeof(kAp) - 1;
  return n >= kApLen && strcasecmp(buf + n - kApLen, kAp) == 0 &&
         (n == kApLen || buf[n - kApLen - 1] == ':');
}

bool headerValue(httpd_req_t* req, const char* name, std::string& out) {
  char buf[256];
  if (httpd_req_get_hdr_value_str(req, name, buf, sizeof(buf)) != ESP_OK) return false;
  out = buf;
  return true;
}

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

bool contentTypeJsonOk(httpd_req_t* req) {
  std::string ct;
  if (!headerValue(req, "Content-Type", ct)) return false;
  if (ct.compare(0, 16, "application/json") != 0) return false;
  return ct.size() == 16 || ct[16] == ';' || ct[16] == ' ' || ct[16] == '\t';
}

// Each nesting level costs ~400 B of stack in the recursive parser, so bodies are parsed
// with the smallest depth their schema needs (kSmallBodyDepth for flat requests).
constexpr size_t kSmallBodyDepth = 4;
constexpr size_t kConfigBodyDepth = 8;

bool parseJsonBody(httpd_req_t* req, const std::string& body, alert_core::JsonValue& out,
                   size_t max_depth = kConfigBodyDepth) {
  alert_core::JsonParseError perr;
  if (!alert_core::jsonParse(body, out, perr, max_depth)) {
    sendError(req, 400, errc::kInvalidRequest,
              std::string("invalid JSON at offset ") + std::to_string(perr.offset) + ": " +
                  perr.message);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
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

bool requireBearer(httpd_req_t* req, std::string& token_out, bool activity = true) {
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
  if (activity) {
    g_last_authenticated_ms.store(esp_timer_get_time() / 1000, std::memory_order_relaxed);
    ProvisionPortal::instance().notePortalActivity();
  }
  return true;
}

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

bool rateLimit(httpd_req_t* req, const std::string& key, uint32_t window_s, uint32_t limit) {
  uint32_t retry = 0;
  if (AuthManager::instance().consumeBudget(key, window_s, limit, retry)) return true;
  ADMIN_LOG_WARN("rate limited: key=%s", key.c_str());
  sendError(req, 429, errc::kTooManyRequests, "rate limit exceeded", nullptr, retry);
  return false;
}

alert_core::JsonValue::Object& mutableItems(alert_core::JsonValue& obj) {
  return const_cast<alert_core::JsonValue::Object&>(obj.objectItems());
}

FieldErrors fieldErr(const std::string& field, const std::string& msg) {
  return FieldErrors{{field, msg}};
}

bool jsonStringField(const alert_core::JsonValue& obj, const char* key, std::string& out) {
  const alert_core::JsonValue* v = obj.find(key);
  if (v == nullptr || v->type() != alert_core::JsonValue::Type::String) return false;
  out = v->stringValue();
  return true;
}

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
  if (!parseJsonBody(req, body, root, kSmallBodyDepth)) return ESP_OK;
  std::string password;
  if (root.type() != alert_core::JsonValue::Type::Object ||
      !jsonStringField(root, "password", password)) {
    return sendError(req, 400, errc::kInvalidRequest, "password (string) required");
  }

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
  {
    const PowerSnapshot power = powerSnapshot();
    if (power.valid && !power.mains) {
      const uint32_t minutes =
          settings::DeviceSettings::instance().snapshot().values.keep_awake_min;
      const int64_t now_ms = esp_timer_get_time() / 1000;
      if (keepAwakeUntilMs() < keep_awake::deadlineMs(now_ms, minutes)) {
        setKeepAwakeMinutes(minutes);
      }
    }
  }
  using alert_core::JsonValue;
  return sendJson(req, 200,
                  ::eego::json::ObjectWriter(3)
                      .string("token", token)
                      .string("token_type", "Bearer")
                      .integer("expires_in", static_cast<int64_t>( AuthManager::instance().sessionTtl()))
                      .take()
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
  if (!parseJsonBody(req, body, root, kSmallBodyDepth)) return ESP_OK;
  std::string old_pw, new_pw;
  if (root.type() != alert_core::JsonValue::Type::Object ||
      !jsonStringField(root, "old_password", old_pw) ||
      !jsonStringField(root, "new_password", new_pw)) {
    return sendError(req, 400, errc::kInvalidRequest,
                     "old_password and new_password (string) required");
  }
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
// ---------------------------------------------------------------------------
esp_err_t h_alerts_ingest(httpd_req_t* req) {
  if (!requireIngestKey(req)) return ESP_OK;
  if (!rateLimit(req, "alerts", 60, 30)) return ESP_OK;
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
          ::eego::json::ObjectWriter(2)
              .integer("created", r.created)
              .integer("updated", r.updated)
              .take()
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
                  ::eego::json::ObjectWriter(3)
                      .value("alerts", ::eego::json::JsonValue(JsonValue::makeArray(std::move(arr))))
                      .integer("total", static_cast<int64_t>(total))
                      .integer("snapshot_seq", snap ? static_cast<int64_t>(snap->seq) : 0)
                      .take()
                      .toString());
}

bool alertIdFromPath(const char* path, std::string& id, const char** rest) {
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
                      ::eego::json::ObjectWriter(2)
                          .boolean("ok", true)
                          .integer("affected", static_cast<int64_t>(r.affected))
                          .take()
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
// ---------------------------------------------------------------------------
alert_core::JsonValue keepAwakeJson() {
  using alert_core::JsonValue;
  const int64_t now_ms = esp_timer_get_time() / 1000;
  const uint32_t remaining = keep_awake::remainingS(now_ms, keepAwakeUntilMs());
  const int64_t sys = static_cast<int64_t>(time(nullptr));
  const int64_t until =
      remaining > 0 && sys >= static_cast<int64_t>(net::kPlausibleUnixFloor)
          ? sys + static_cast<int64_t>(remaining)
          : 0;
  return ::eego::json::ObjectWriter(4)
      .boolean("active", remaining > 0)
      .integer("remaining_s", remaining)
      .integer("until", until)
      .integer("default_min", settings::DeviceSettings::instance().snapshot().values.keep_awake_min)
      .take();
}

alert_core::JsonValue screenJson();

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
                    ::eego::json::ObjectWriter(4)
                        .string("fw_version", app ? app->version : "unknown")
                        .string("fw_sha256", sha_hex)
                        .integer("uptime_s", esp_timer_get_time() / 1000000)
                        .string("mode", mode)
                        .take());

  net::NetStatusSnapshot ns;
  const bool ns_ok = net::NetStack::instance().queryStatus(ns);
  JsonValue::Object netobj;
  if (ns_ok) {
    netobj.emplace_back("ssid", JsonValue::makeString(ns.wifi.ssid));
    netobj.emplace_back("ip", JsonValue::makeString(ns.wifi.ip));
    netobj.emplace_back("rssi", JsonValue::makeInt(ns.wifi.rssi));
    netobj.emplace_back("wifi_state", JsonValue::makeInt(ns.wifi.state));
    netobj.emplace_back("last_disconnect_reason",
                        JsonValue::makeInt(ns.wifi.last_disconnect_reason));
    netobj.emplace_back("reconnect_attempts", JsonValue::makeInt(ns.wifi.reconnect_attempts));
    netobj.emplace_back("disconnects", JsonValue::makeInt(ns.wifi.disconnects));
    netobj.emplace_back("rssi_min_10m", JsonValue::makeInt(ns.wifi.rssi_samples ? ns.wifi.rssi_min : 0));
    netobj.emplace_back("rssi_avg_10m", JsonValue::makeInt(ns.wifi.rssi_samples ? ns.wifi.rssi_avg : 0));
    netobj.emplace_back("beacon_timeouts", JsonValue::makeInt(ns.wifi.beacon_timeouts));
    netobj.emplace_back("roams", JsonValue::makeInt(ns.wifi.roams));
    netobj.emplace_back("last_dhcp_ms", JsonValue::makeInt(ns.wifi.last_dhcp_ms));
    netobj.emplace_back("tx_power_qdbm", JsonValue::makeInt(ns.wifi.tx_power_qdbm));
  } else {
    netobj.emplace_back("ssid", JsonValue::makeString(net::WifiManager::instance().currentSsid()));
    netobj.emplace_back("ip", JsonValue::makeString(net::WifiManager::instance().currentIp()));
    netobj.emplace_back("rssi", JsonValue::makeInt(net::WifiManager::instance().rssi()));
    netobj.emplace_back("wifi_state",
                        JsonValue::makeInt(static_cast<int>(net::WifiManager::instance().state())));
  }
  {
    const int rssi = ns_ok ? ns.wifi.rssi : net::WifiManager::instance().rssi();
    const net::WifiSignal q = net::wifiSignalQuality(rssi);
    netobj.emplace_back("signal", JsonValue::makeString(net::wifiSignalName(q)));
    netobj.emplace_back("signal_label", JsonValue::makeString(net::wifiSignalLabel(q, false)));
    netobj.emplace_back("signal_bars", JsonValue::makeInt(net::wifiSignalBars(q)));
    netobj.emplace_back("signal_hint", JsonValue::makeString(
                                           q == net::WifiSignal::Weak ? net::kWifiWeakSignalHint : ""));
  }
  netobj.emplace_back("ap_active", JsonValue::makeBool(ProvisionPortal::instance().apActive()));
  root.emplace_back("network", JsonValue::makeObject(std::move(netobj)));

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
                    ::eego::json::ObjectWriter(2)
                        .integer("internal_free", static_cast<int64_t>( heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)))
                        .integer("psram_free", static_cast<int64_t>( heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)))
                        .take());
  root.emplace_back("storage",
                    ::eego::json::ObjectWriter(2)
                        .boolean("sd_ready", ::eego::SdCard::instance().ready())
                        .boolean("config_valid", ConfigStore::instance().valid())
                        .take());

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

  root.emplace_back("security",
                    ::eego::json::ObjectWriter(3)
                        .integer("sessions", static_cast<int64_t>(AuthManager::instance().sessionCount()))
                        .string("ingest_key_masked", AuthManager::instance().maskedIngestKey())
                        .boolean("provisioned", AuthManager::instance().hasPassword())
                        .take());

  root.emplace_back("screen", screenJson());
  root.emplace_back("keep_awake", keepAwakeJson());
  const PowerSnapshot power = powerSnapshot();
  if (power.valid) {
    root.emplace_back(
        "power",
        ::eego::json::ObjectWriter(14)
            .string("source", power.source)
            .string("source_reason", power.source_reason)
            .string("power_mode", power.power_mode)
            .string("mode", power.mode)
            .integer("battery_pct", power.battery_pct)
            .integer("battery_mv", power.battery_mv)
            .boolean("charging", power.charging)
            .boolean("usb_host", power.usb_host)
            .integer("next_wake", static_cast<int64_t>(power.next_wake))
            .integer("next_refresh", static_cast<int64_t>(power.next_refresh))
            .integer("sleep_in_s", power.sleep_in_s)
            .string("last_wake_reason", power.last_wake_reason)
            .integer("sleeps_since_boot", power.sleeps_since_boot)
            .integer("last_screen_update", static_cast<int64_t>(power.last_screen_update))
            .take());
  }

  return sendJson(req, 200, JsonValue::makeObject(std::move(root)).toString());
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
alert_core::JsonValue screenJson() {
  using alert_core::JsonValue;
  const ScreenState state = screenState();
  return ::eego::json::ObjectWriter(6)
      .boolean("available", state.available)
      .integer("index", state.index)
      .integer("count", state.count)
      .string("page_id", state.page_id)
      .boolean("shown", state.shown)
      .integer("rotate_min", state.rotate_min)
      .take();
}

esp_err_t h_screen_post(httpd_req_t* req) {
  using alert_core::JsonValue;
  if (!contentTypeJsonOk(req)) {
    return sendError(req, 415, errc::kUnsupportedMediaType, "JSON content type required");
  }
  std::string body;
  if (readBodyBudget(req, kBodyBudgetConfirm, body) != 0) return ESP_OK;
  JsonValue root;
  if (!parseJsonBody(req, body, root, kSmallBodyDepth)) return ESP_OK;
  const JsonValue* index = root.find("index");
  const JsonValue* step = root.find("step");
  const bool index_ok = index != nullptr && index->type() == JsonValue::Type::Int;
  const bool step_ok = step != nullptr && step->type() == JsonValue::Type::Int;
  if (index_ok == step_ok || (index != nullptr && !index_ok) || (step != nullptr && !step_ok)) {
    FieldErrors errs = fieldErr("index", "give exactly one of integer index (0-based) or step");
    return sendError(req, 400, errc::kValidationFailed, "validation failed", &errs);
  }
  const ScreenState state = screenState();
  if (!state.available || state.count == 0) {
    return sendError(req, 409, errc::kConflict, "no dashboard loaded");
  }
  const int64_t target = device_screen::resolveTarget(
      index_ok, index_ok ? index->intValue() : 0, step_ok ? step->intValue() : 0, state.index,
      state.count);
  if (target < 0) {
    FieldErrors errs = index_ok ? fieldErr("index", "must be in [0, " +
                                                        std::to_string(state.count - 1) + "]")
                                : fieldErr("step", "must be a nonzero integer in [-8, 8]");
    return sendError(req, 400, errc::kValidationFailed, "validation failed", &errs);
  }
  requestScreen(static_cast<uint32_t>(target));
  ADMIN_LOG_INFO("screen %d/%u via api", static_cast<int>(target + 1),
                 static_cast<unsigned>(state.count));
  return sendJson(req, 200, ::eego::json::ObjectWriter(2)
      .integer("index", target)
      .integer("count", state.count)
      .take()
      .toString());
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
std::mutex g_time_mutex;
TimeSetter g_time_setter;

esp_err_t h_device_time(httpd_req_t* req) {
  using alert_core::JsonValue;
  if (!contentTypeJsonOk(req)) {
    return sendError(req, 415, errc::kUnsupportedMediaType, "JSON content type required");
  }
  std::string body;
  if (readBodyBudget(req, kBodyBudgetConfirm, body) != 0) return ESP_OK;
  JsonValue root;
  if (!parseJsonBody(req, body, root, kSmallBodyDepth)) return ESP_OK;
  const JsonValue* unix_ms = root.find("unix_ms");
  // 2020-01-01 .. 2100-01-01
  if (unix_ms == nullptr || unix_ms->type() != JsonValue::Type::Int ||
      unix_ms->intValue() < 1577836800000LL || unix_ms->intValue() > 4102444800000LL) {
    FieldErrors errs = fieldErr("unix_ms", "must be a Unix time in milliseconds");
    return sendError(req, 400, errc::kValidationFailed, "validation failed", &errs);
  }
  TimeSetter setter;
  {
    std::lock_guard<std::mutex> lock(g_time_mutex);
    setter = g_time_setter;
  }
  if (!setter) return sendError(req, 503, errc::kUnavailable, "time setting unavailable");
  const TimeSetResult result = setter(unix_ms->intValue());
  if (result.applied) {
    ADMIN_LOG_INFO("time set from browser (drift %llds)", static_cast<long long>(result.drift_s));
  }
  return sendJson(req, 200, ::eego::json::ObjectWriter(3)
      .boolean("applied", result.applied)
      .integer("drift_s", result.drift_s)
      .boolean("sntp_synced", result.sntp_synced)
      .take()
      .toString());
}

esp_err_t h_keep_awake_get(httpd_req_t* req) {
  return sendJson(req, 200, keepAwakeJson().toString());
}

esp_err_t h_keep_awake_post(httpd_req_t* req) {
  using alert_core::JsonValue;
  if (!contentTypeJsonOk(req)) {
    return sendError(req, 415, errc::kUnsupportedMediaType, "JSON content type required");
  }
  std::string body;
  if (readBodyBudget(req, kBodyBudgetConfirm, body) != 0) return ESP_OK;
  JsonValue root;
  if (!parseJsonBody(req, body, root, kSmallBodyDepth)) return ESP_OK;
  const JsonValue* minutes = root.find("minutes");
  if (minutes == nullptr || minutes->type() != JsonValue::Type::Int ||
      !keep_awake::validRequestMinutes(minutes->intValue())) {
    FieldErrors errs = fieldErr("minutes", "integer in [0, 240] required (0 = cancel)");
    return sendError(req, 400, errc::kValidationFailed, "validation failed", &errs);
  }
  setKeepAwakeMinutes(static_cast<uint32_t>(minutes->intValue()));
  ADMIN_LOG_INFO("keep-awake %d min via api", static_cast<int>(minutes->intValue()));
  return sendJson(req, 200, keepAwakeJson().toString());
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
    arr.push_back(::eego::json::ObjectWriter(8)
        .string("id", d.id)
        .string("name_zh", d.name_zh)
        .string("name_en", d.name_en)
        .string("severity_default", alert_core::severityName(d.severity_default))
        .string("group", d.group)
        .integer("ttl_default_s", static_cast<int64_t>(d.ttl_default_s))
        .boolean("muted_default", d.muted_default)
        .boolean("overridden", reg->findOverride(b.id) != nullptr)
        .take());
  }
  return sendJson(req, 200,
                  ::eego::json::ObjectWriter(1)
                      .value("types", ::eego::json::JsonValue(JsonValue::makeArray(std::move(arr))))
                      .take()
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
                  ::eego::json::ObjectWriter(6)
                      .string("id", d.id)
                      .string("name_zh", d.name_zh)
                      .string("name_en", d.name_en)
                      .string("severity_default", alert_core::severityName(d.severity_default))
                      .integer("ttl_default_s", static_cast<int64_t>(d.ttl_default_s))
                      .boolean("muted_default", d.muted_default)
                      .take()
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
    arr.push_back(::eego::json::ObjectWriter(5)
        .integer("ts", static_cast<int64_t>(e.ts))
        .boolean("unsynced", e.unsynced)
        .string("level", logLevelName(e.level))
        .integer("seq", static_cast<int64_t>(e.seq))
        .string("msg", e.msg)
        .take());
  }
  return sendJson(req, 200,
                  ::eego::json::ObjectWriter(1)
                      .value("entries", ::eego::json::JsonValue(JsonValue::makeArray(std::move(arr))))
                      .take()
                      .toString());
}

}  // namespace

}  // namespace eego::admin

namespace eego::admin {
namespace {
// ---------------------------------------------------------------------------
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
  const JsonValue power = ::eego::json::ObjectWriter(7)
      .string("last_wake_reason", wakeReasonName())
      .integer("next_scheduled_wake", static_cast<int64_t>(wake_at))
      .boolean("deep_sleep_http_reachable", false)
      .boolean("deep_sleep_ble_reachable", false)
      .boolean("recovery_ap_active", ProvisionPortal::instance().apActive())
      .integer("recovery_window_remaining_s", ProvisionPortal::instance().windowRemainingMs() / 1000)
      .boolean("auto_sleep_suspended", ProvisionPortal::instance().apActive() || ble_state.scanning || ble_state.pairing_passkey_valid)
      .take();
  const JsonValue capabilities = ::eego::json::ObjectWriter(8)
      .boolean("frontlight", cap.frontlight)
      .boolean("battery", cap.battery)
      .boolean("bluetooth", cap.bluetooth)
      .boolean("ble_admin", cap.ble_admin)
      .boolean("ble_observer", cap.ble_observer)
      .boolean("display", cap.display)
      .boolean("sd", cap.sd)
      .boolean("rtc", cap.rtc)
      .take();
  const JsonValue ble_json = ::eego::json::ObjectWriter(26)
      .boolean("compiled", ble_state.compiled)
      .boolean("stack_active", ble_state.stack_active)
      .string("role", ble::roleName(ble_state.role))
      .boolean("admin_enabled", snap.values.ble_admin_enabled)
      .boolean("admin_allowed", ble_state.admin_allowed)
      .boolean("advertising", ble_state.advertising)
      .string("link", ble::linkStateName(ble_state.link))
      .boolean("pairing_locked", ble_state.pairing_locked)
      .integer("bonds", ble_state.bonds)
      .integer("att_mtu", ble_state.att_mtu)
      .string("device_name", ble_state.device_name)
      .integer("tunnel_messages", ble_state.tunnel_messages)
      .integer("tunnel_errors", ble_state.tunnel_errors)
      .boolean("observer_enabled", snap.values.ble_observer_enabled)
      .integer("observer_remaining_s", ble_state.observer_remaining_s)
      .boolean("recovery_ap_active", ble_state.recovery_ap_active)
      .boolean("deep_sleep_realtime", false)
      .integer("dropped_advertisements", ble_state.dropped_advertisements)
      .integer("duplicate_advertisements", ble_state.duplicate_advertisements)
      .integer("malformed_advertisements", ble_state.malformed_advertisements)
      .integer("emitted_events", ble_state.emitted_events)
      .integer("ingest_errors", ble_state.ingest_errors)
      .integer("diram_free_bytes", ble_state.diram_free_bytes)
      .integer("diram_min_free_bytes", ble_state.diram_min_free_bytes)
      .integer("psram_free_bytes", ble_state.psram_free_bytes)
      .string("last_error", ble_state.last_error)
      .take();
  return sendJson(req, 200,
                  ::eego::json::ObjectWriter(6)
                      .integer("schema_version", settings::kSchemaVersion)
                      .integer("revision", snap.revision)
                      .value("settings", ::eego::json::JsonValue(settings::toJson(snap.values)))
                      .value("capabilities", ::eego::json::JsonValue(capabilities))
                      .value("power", ::eego::json::JsonValue(power))
                      .value("ble", ::eego::json::JsonValue(ble_json))
                      .take()
                      .toString());
}

esp_err_t h_ble_unbind(httpd_req_t* req) {
  if (!contentTypeJsonOk(req)) {
    return sendError(req, 415, errc::kUnsupportedMediaType, "JSON content type required");
  }
  std::string body;
  if (readBodyBudget(req, kBodyBudgetLogin, body) != 0) return ESP_OK;
  alert_core::JsonValue root;
  if (!parseJsonBody(req, body, root, kSmallBodyDepth)) return ESP_OK;
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

// Validates and applies a settings patch. Returns false when a response (error) has
// already been sent; `result` then holds its send status. Kept out of line so its frame
// (patch DOM, candidate settings) is gone before h_settings_get builds the reply.
__attribute__((noinline)) bool applySettingsPatch(httpd_req_t* req, esp_err_t& result) {
  result = ESP_OK;
  if (!contentTypeJsonOk(req)) {
    result = sendError(req, 415, errc::kUnsupportedMediaType, "JSON content type required");
    return false;
  }
  std::string body;
  const int rb = readBodyBudget(req, kBodyBudgetConfig, body);
  if (rb != 0) return false;
  alert_core::JsonValue patch;
  if (!parseJsonBody(req, body, patch)) return false;

  const settings::Snapshot snap = settings::DeviceSettings::instance().snapshot();
  settings::Settings candidate;
  settings::FieldErrors settings_errors;
  if (!settings::mergePatch(snap.values, patch, candidate, settings_errors)) {
    FieldErrors errors;
    for (const auto& error : settings_errors) errors.push_back({error.field, error.message});
    result = sendError(req, 400, errc::kValidationFailed, "settings validation failed", &errors);
    return false;
  }
  if ((settings::changedFields(snap.values, candidate) & settings::kChangedFrontlight) != 0 &&
      !snap.capabilities.frontlight) {
    FieldErrors errors = fieldErr("frontlight_enabled", "frontlight hardware is unavailable");
    result = sendError(req, 400, errc::kValidationFailed, "settings validation failed", &errors);
    return false;
  }
  if (candidate.ble_observer_enabled && !snap.capabilities.ble_observer) {
    FieldErrors errors = fieldErr("ble_observer_enabled", "BLE observer is unavailable");
    result = sendError(req, 400, errc::kValidationFailed, "settings validation failed", &errors);
    return false;
  }

  AdminCommand cmd;
  cmd.type = AdminCommandType::ApplySettings;
  cmd.settings_patch = std::move(patch);
  AdminResult res;
  if (!runCommand(req, cmd, res)) return false;
  ADMIN_LOG_INFO("typed device settings updated");
  return true;
}

esp_err_t h_settings_put(httpd_req_t* req) {
  esp_err_t result = ESP_OK;
  if (!applySettingsPatch(req, result)) return result;
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
  if (!parseJsonBody(req, body, root, kSmallBodyDepth)) return ESP_OK;
  std::string ssid, password;
  if (root.type() != alert_core::JsonValue::Type::Object ||
      !jsonStringField(root, "ssid", ssid)) {
    return sendError(req, 400, errc::kInvalidRequest, "ssid (string) required");
  }
  jsonStringField(root, "password", password);
  FieldErrors errs;
  if (ssid.empty() || ssid.size() > 32) {
    errs.push_back({"ssid", "must be 1..32 bytes"});
  }
  if (!net::validWifiPassword(password)) {
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
  if (!parseJsonBody(req, body, root, kSmallBodyDepth)) return ESP_OK;
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

std::string wifiListJson() {
  using alert_core::JsonValue;
  std::vector<net::WifiAccount> accounts;
  net::CredentialStore::loadWifiAccounts(accounts);
  JsonValue::Array networks;
  for (size_t i = 0; i < accounts.size(); i++) {
    networks.push_back(::eego::json::ObjectWriter(3)
        .string("ssid", accounts[i].ssid)
        .boolean("has_password", !accounts[i].password.empty())
        .string("pass_ref", "nvs:wifi/" + std::to_string(i))
        .take());
    net::CredentialStore::secureErase(accounts[i].password);
  }
  net::NetStatusSnapshot ns;
  if (!net::NetStack::instance().queryStatus(ns)) {
    auto& wifi = net::WifiManager::instance();
    ns.wifi.ssid = wifi.currentSsid();
    ns.wifi.ip = wifi.currentIp();
    ns.wifi.rssi = wifi.rssi();
    ns.wifi.state = static_cast<int>(wifi.state());
    ns.wifi.last_disconnect_reason = wifi.lastDisconnectReason();
    ns.wifi.driver_restarts = wifi.driverRestarts();
  }
  const std::string& ssid = ns.wifi.ssid;
  const std::string& ip = ns.wifi.ip;
  const int rssi = ns.wifi.rssi;
  const int state = ns.wifi.state;
  const bool connected = state == static_cast<int>(net::WifiState::Connected);
  int index = -1;
  for (size_t i = 0; connected && i < accounts.size(); i++) {
    if (accounts[i].ssid == ssid) index = static_cast<int>(i);
  }
  JsonValue::Object current;
  current.emplace_back("state",
                       JsonValue::makeString(net::wifiStateName(static_cast<net::WifiState>(state))));
  current.emplace_back("connected", JsonValue::makeBool(connected));
  current.emplace_back("ssid", JsonValue::makeString(connected ? ssid : ""));
  current.emplace_back("rssi", JsonValue::makeInt(connected ? rssi : 0));
  current.emplace_back("ip", JsonValue::makeString(connected ? ip : ""));
  current.emplace_back("index", JsonValue::makeInt(index));
  current.emplace_back("last_disconnect_reason",
                       JsonValue::makeInt(ns.wifi.last_disconnect_reason));
  current.emplace_back("reconnect_attempts", JsonValue::makeInt(ns.wifi.reconnect_attempts));
  current.emplace_back("disconnects", JsonValue::makeInt(ns.wifi.disconnects));
  current.emplace_back("driver_restarts", JsonValue::makeInt(ns.wifi.driver_restarts));
  current.emplace_back("offline_ms", JsonValue::makeInt(ns.wifi.offline_ms));
  current.emplace_back("reconnect_backoff_ms", JsonValue::makeInt(ns.wifi.reconnect_backoff_ms));
  {
    const net::WifiSignal q = net::wifiSignalQuality(connected ? rssi : 0);
    current.emplace_back("signal", JsonValue::makeString(net::wifiSignalName(q)));
    current.emplace_back("signal_bars", JsonValue::makeInt(net::wifiSignalBars(q)));
    current.emplace_back("signal_hint", JsonValue::makeString(
                                            q == net::WifiSignal::Weak ? net::kWifiWeakSignalHint : ""));
    current.emplace_back("rssi_min_10m",
                         JsonValue::makeInt(ns.wifi.rssi_samples ? ns.wifi.rssi_min : 0));
    current.emplace_back("rssi_avg_10m",
                         JsonValue::makeInt(ns.wifi.rssi_samples ? ns.wifi.rssi_avg : 0));
    current.emplace_back("beacon_timeouts", JsonValue::makeInt(ns.wifi.beacon_timeouts));
    current.emplace_back("roams", JsonValue::makeInt(ns.wifi.roams));
    current.emplace_back("last_dhcp_ms", JsonValue::makeInt(ns.wifi.last_dhcp_ms));
  }
  return ::eego::json::ObjectWriter(3)
      .value("networks", ::eego::json::JsonValue(JsonValue::makeArray(std::move(networks))))
      .integer("max", net::kMaxWifiNetworks)
      .value("current", ::eego::json::JsonValue(JsonValue::makeObject(std::move(current))))
      .take()
      .toString();
}

esp_err_t h_wifi_list_get(httpd_req_t* req) { return sendJson(req, 200, wifiListJson()); }

esp_err_t h_wifi_list_put(httpd_req_t* req) {
  using alert_core::JsonValue;
  if (!contentTypeJsonOk(req)) {
    return sendError(req, 415, errc::kUnsupportedMediaType, "JSON content type required");
  }
  std::string body;
  const int rb = readBodyBudget(req, kBodyBudgetSmall, body);
  if (rb != 0) return ESP_OK;
  JsonValue root;
  if (!parseJsonBody(req, body, root, kSmallBodyDepth)) return ESP_OK;
  net::CredentialStore::secureErase(body);
  const JsonValue* list = root.type() == JsonValue::Type::Object ? root.find("networks") : nullptr;
  if (list == nullptr || list->type() != JsonValue::Type::Array) {
    return sendError(req, 400, errc::kInvalidRequest, "networks (array) required");
  }
  auto entries = std::make_shared<std::vector<net::WifiListEntry>>();
  for (size_t i = 0; i < list->arrayItems().size(); i++) {
    const JsonValue& item = list->arrayItems()[i];
    const std::string prefix = "networks[" + std::to_string(i) + "]";
    if (item.type() != JsonValue::Type::Object) {
      const FieldErrors errs = fieldErr(prefix, "must be an object");
      return sendError(req, 400, errc::kValidationFailed, "validation failed", &errs);
    }
    net::WifiListEntry entry;
    if (!jsonStringField(item, "ssid", entry.ssid)) {
      const FieldErrors errs = fieldErr(prefix + ".ssid", "string required");
      return sendError(req, 400, errc::kValidationFailed, "validation failed", &errs);
    }
    const JsonValue* password = item.find("password");
    if (password == nullptr) {
      entry.keep_password = true;
    } else if (password->type() != JsonValue::Type::String) {
      const FieldErrors errs = fieldErr(prefix + ".password", "must be a string");
      return sendError(req, 400, errc::kValidationFailed, "validation failed", &errs);
    } else if (password->stringValue() == net::kWifiPasswordKeep) {
      entry.keep_password = true;
    } else {
      entry.password = password->stringValue();
    }
    jsonStringField(item, "previous_ssid", entry.previous_ssid);
    entries->push_back(std::move(entry));
  }
  {
    std::vector<net::WifiAccount> current, merged;
    net::CredentialStore::loadWifiAccounts(current);
    net::WifiListError list_error;
    const bool ok = net::applyWifiListEdit(current, *entries, merged, list_error);
    for (auto& a : current) net::CredentialStore::secureErase(a.password);
    for (auto& a : merged) net::CredentialStore::secureErase(a.password);
    if (!ok) {
      const FieldErrors errs = fieldErr(list_error.field, list_error.message);
      return sendError(req, 400, errc::kValidationFailed, "validation failed", &errs);
    }
  }
  AdminCommand cmd;
  cmd.type = AdminCommandType::ApplyWifi;
  cmd.wifi_list = entries;
  AdminResult res;
  const bool ok = runCommand(req, cmd, res);
  for (auto& e : *entries) net::CredentialStore::secureErase(e.password);
  if (!ok) return ESP_OK;
  ADMIN_LOG_INFO("wifi list replaced via api: networks=%u", static_cast<unsigned>(entries->size()));
  return sendJson(req, 200, wifiListJson());
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
esp_err_t h_source_credentials_get(httpd_req_t* req) {
  std::vector<net::SourceCredentialMetadata> credentials;
  if (net::CredentialStore::listSourceCredentials(credentials) != ESP_OK) {
    return sendError(req, 503, errc::kUnavailable, "source credential store unavailable");
  }
  using alert_core::JsonValue;
  JsonValue::Array items;
  for (const auto& credential : credentials) {
    items.push_back(::eego::json::ObjectWriter(2)
        .string("key", credential.key)
        .string("ref", credential.ref)
        .take());
  }
  return sendJson(req, 200,
                  ::eego::json::ObjectWriter(2)
                      .string("namespace", "source")
                      .value("credentials", ::eego::json::JsonValue(JsonValue::makeArray(std::move(items))))
                      .take()
                      .toString());
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
  if (!parseJsonBody(req, body, root, kSmallBodyDepth)) {
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
                  ::eego::json::ObjectWriter(3)
                      .string("key", key)
                      .string("ref", net::CredentialStore::sourceRef(key))
                      .boolean("deleted", remove)
                      .take()
                      .toString());
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
esp_err_t h_reboot(httpd_req_t* req) {
  if (!contentTypeJsonOk(req)) {
    return sendError(req, 415, errc::kUnsupportedMediaType, "JSON content type required");
  }
  std::string body;
  const int rb = readBodyBudget(req, kBodyBudgetConfirm, body);
  if (rb != 0) return ESP_OK;
  alert_core::JsonValue root;
  if (!parseJsonBody(req, body, root, kSmallBodyDepth)) return ESP_OK;
  std::string confirm;
  if (!jsonStringField(root, "confirm", confirm) || confirm != "REBOOT") {
    FieldErrors errs = fieldErr("confirm", "must be \"REBOOT\"");
    return sendError(req, 400, errc::kValidationFailed, "validation failed", &errs);
  }
  AdminCommand cmd;
  cmd.type = AdminCommandType::Reboot;
  cmd.delay_ms = 2000;
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
  if (!parseJsonBody(req, body, root, kSmallBodyDepth)) return ESP_OK;
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
  return sendJson(req, 200,
                  ::eego::json::ObjectWriter(1)
                      .string("ingest_key", key)
                      .take()
                      .toString());
}

// ---------------------------------------------------------------------------
// GET / and /loader.js: zero-data loader for the externally hosted React app.
// The page CSP allows scripts/styles from EEGO_WEB_APP_ORIGIN only.
// ---------------------------------------------------------------------------
esp_err_t redirectToWifiDiag(httpd_req_t* req) {
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/wifi-diag");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_send(req, "", 0);
  return ESP_OK;
}

esp_err_t h_root_page(httpd_req_t* req) {
  if (ProvisionPortal::instance().isApFallbackActive() && viaSetupHotspot(req)) {
    return redirectToWifiDiag(req);
  }
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
// ---------------------------------------------------------------------------
struct AsyncJob {
  httpd_req_t* req;
  char path[160];
};
QueueHandle_t g_async_queue = nullptr;

void asyncWorker(void*) {
  AsyncJob job;
  for (;;) {
    if (xQueueReceive(g_async_queue, &job, portMAX_DELAY) != pdTRUE) continue;
    internal::handleDashboardApi(job.req, job.path);
    httpd_req_async_handler_complete(job.req);
  }
}

// Every dashboard write (and the slow source/preview calls) runs on the 20 KB worker:
// they parse and validate whole dashboards, which is too deep for the httpd stack.
bool asyncDashboardPath(httpd_req_t* req, const char* path) {
  return req->method != HTTP_GET || strcmp(path, "dashboard/sources/test") == 0 ||
         strcmp(path, "dashboard/sources/probe") == 0 ||
         strcmp(path, "dashboard/render-preview") == 0;
}

esp_err_t handleDashboardMaybeAsync(httpd_req_t* req, const std::string& path) {
  if (!asyncDashboardPath(req, path.c_str()) || path.size() >= sizeof(AsyncJob::path)) {
    return internal::handleDashboardApi(req, path.c_str());
  }
  if (g_async_queue == nullptr) {
    g_async_queue = xQueueCreate(4, sizeof(AsyncJob));
    if (g_async_queue == nullptr ||
        xTaskCreate(&asyncWorker, "admin_async", 20480, nullptr, 5, nullptr) != pdPASS) {
      if (g_async_queue != nullptr) vQueueDelete(g_async_queue);
      g_async_queue = nullptr;
      return internal::handleDashboardApi(req, path.c_str());
    }
  }
  AsyncJob job{};
  strncpy(job.path, path.c_str(), sizeof(job.path) - 1);
  if (httpd_req_async_handler_begin(req, &job.req) != ESP_OK) {
    return internal::handleDashboardApi(req, path.c_str());
  }
  if (xQueueSend(g_async_queue, &job, 0) != pdTRUE) {
    sendError(job.req, 503, errc::kUnavailable, "device busy, try again");
    httpd_req_async_handler_complete(job.req);
  }
  return ESP_OK;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
esp_err_t apiDispatch(httpd_req_t* req, const char* path) {
  if (!hostOriginOk(req)) {
    ADMIN_LOG_WARN("host/origin rejected");
    return sendError(req, 403, errc::kForbidden, "forbidden");
  }

  const auto m = req->method;

  if (m == HTTP_POST && strcmp(path, "auth/login") == 0) {
    return h_login(req);
  }

  if (m == HTTP_POST && strcmp(path, "alerts") == 0) {
    return h_alerts_ingest(req);
  }

  std::string token;
  auto bearer = [&]() { return requireBearer(req, token); };

  if (strcmp(path, "dashboard") == 0 || strncmp(path, "dashboard/", 10) == 0) {
    if (!bearer()) return ESP_OK;
    const std::string canonical_path = internal::canonicalDashboardApiPath(path);
    if (m == HTTP_GET) {
      if (!rateLimit(req, "get:" + clientIp(req), 60, 300)) return ESP_OK;
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
    return handleDashboardMaybeAsync(req, canonical_path);
  }

  if (m == HTTP_GET) {
    const bool poll = strcmp(path, "status") == 0 || strcmp(path, "wifi/diag") == 0;
    if (!rateLimit(req, (poll ? "poll:" : "get:") + clientIp(req), 60, poll ? 120 : 300)) {
      return ESP_OK;
    }
    if (strcmp(path, "status") == 0) {
      if (!requireBearer(req, token, false)) return ESP_OK;
      return h_status(req);
    }
    if (strcmp(path, "wifi/diag") == 0) {
      if (!requireBearer(req, token, false)) return ESP_OK;
      return sendJson(req, 200, wifi_diag::diagJson());
    }
    if (strcmp(path, "device/keep-awake") == 0) {
      if (!requireBearer(req, token, false)) return ESP_OK;
      return h_keep_awake_get(req);
    }
    if (strcmp(path, "device/screen") == 0) {
      if (!requireBearer(req, token, false)) return ESP_OK;
      return sendJson(req, 200, screenJson().toString());
    }
    if (strcmp(path, "types") == 0) {
      if (!bearer()) return ESP_OK;
      return h_types_list(req);
    }
    if (strcmp(path, "system/logs") == 0) {
      if (!bearer()) return ESP_OK;
      return h_logs(req);
    }
    if (strcmp(path, "system/device-log") == 0) {
      if (!bearer()) return ESP_OK;
      size_t bytes = 16 * 1024;
      char q[32] = {};
      char v[12] = {};
      if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
          httpd_query_key_value(q, "bytes", v, sizeof(v)) == ESP_OK) {
        const long n = strtol(v, nullptr, 10);
        if (n > 0) bytes = static_cast<size_t>(n);
      }
      const std::string text = devlog::tail(bytes);
      httpd_resp_set_type(req, "text/plain; charset=utf-8");
      httpd_resp_set_hdr(req, "Cache-Control", "no-store");
      return httpd_resp_send(req, text.data(), text.size());
    }
    if (strcmp(path, "system/crash") == 0) {
      if (!bearer()) return ESP_OK;
      return sendJson(req, 200, devlog::crashJson());
    }
    if (strcmp(path, "system/tasks") == 0) {
      if (!bearer()) return ESP_OK;
      return sendJson(req, 200, devlog::tasksJson());
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
    if (strcmp(path, "config/wifi") == 0) {
      if (!bearer()) return ESP_OK;
      return h_wifi_list_get(req);
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
    if (strcmp(path, "wifi/retry") == 0 || strcmp(path, "wifi/scan") == 0) {
      if (!bearer()) return ESP_OK;
      const bool scan = path[5] == 's';
      if (!rateLimit(req, std::string(scan ? "wifi-scan:" : "wifi-retry:") + token.substr(0, 8), 5,
                     1)) {
        return ESP_OK;
      }
      if (scan && !wifi_diag::scanNow()) {
        return sendError(req, 409, errc::kConflict, "wifi busy connecting; showing last scan");
      }
      if (!scan) wifi_diag::retryNow();
      return sendJson(req, 200, "{\"ok\":true}");
    }
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
    if (strcmp(path, "system/crash/clear") == 0) {
      if (!bearer()) return ESP_OK;
      const esp_err_t err = devlog::clearCrash();
      if (err != ESP_OK) return sendError(req, 500, errc::kInternal, esp_err_to_name(err));
      return sendJson(req, 200, "{\"cleared\":true}");
    }
    if (strcmp(path, "device/keep-awake") == 0) {
      if (!bearer()) return ESP_OK;
      if (!rateLimit(req, "keep-awake:" + token.substr(0, 8), 60, 30)) return ESP_OK;
      return h_keep_awake_post(req);
    }
    if (strcmp(path, "device/time") == 0) {
      if (!bearer()) return ESP_OK;
      if (!rateLimit(req, "time:" + token.substr(0, 8), 60, 6)) return ESP_OK;
      return h_device_time(req);
    }
    if (strcmp(path, "device/screen") == 0) {
      if (!bearer()) return ESP_OK;
      if (!rateLimit(req, "screen:" + token.substr(0, 8), 60, 60)) return ESP_OK;
      return h_screen_post(req);
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
    if (strcmp(path, "config/wifi") == 0) {
      if (!bearer()) return ESP_OK;
      if (!rateLimit(req, "wifi-write:" + token.substr(0, 8), 60, 20)) return ESP_OK;
      return h_wifi_list_put(req);
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

esp_err_t wildcardHandler(httpd_req_t* req) {
  const char* uri = req->uri;
  if (req->method == HTTP_GET) {
    if (strcmp(uri, "/") == 0 || strncmp(uri, "/?", 2) == 0) return h_root_page(req);
    if (strcmp(uri, "/loader.js") == 0) return h_loader_js(req);
    if (strcmp(uri, "/wifi-diag") == 0) {
      return sendStatic(req, "text/html; charset=utf-8", wifi_diag::kHtml, "default-src 'self'");
    }
    if (strcmp(uri, "/wifi-diag.js") == 0) {
      return sendStatic(req, "application/javascript; charset=utf-8", wifi_diag::kJs, nullptr);
    }
    if (strcmp(uri, "/wifi-diag.css") == 0) {
      return sendStatic(req, "text/css; charset=utf-8", wifi_diag::kCss, nullptr);
    }
  }
  if (strncmp(uri, "/api/v1/", 8) == 0) {
    std::string path(uri + 8);
    const size_t query = path.find('?');
    if (query != std::string::npos) path.resize(query);
    return apiDispatch(req, path.c_str());
  }
  if (req->method == HTTP_GET && ProvisionPortal::instance().isApFallbackActive() &&
      viaSetupHotspot(req)) {
    return redirectToWifiDiag(req);
  }
  return sendError(req, 404, errc::kNotFound, "resource not found");
}

}  // namespace

// ===========================================================================
// ===========================================================================

esp_err_t startAdminApi(const AdminConfig& cfg) {
  if (g_server != nullptr) return ESP_ERR_INVALID_STATE;
  if (ProvisionPortal::instance().isOpen()) return ESP_ERR_INVALID_STATE;

  // AdminLog / AuthManager / ConfigStore / DashboardStore are initialized
  // exactly once by the composition root (app_main) before this is called.
  g_cfg = cfg;
  internal::setRegistry(cfg.registry);

  const AdminDomainConfig admin = ConfigStore::instance().admin();
  AuthManager::instance().setSessionTtl(admin.session_ttl_s);
  AuthManager::instance().setLockThreshold(admin.login_lock_threshold);

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = cfg.port;
  config.max_uri_handlers = 12;
  config.stack_size = 16384;  // measured handler chains reach ~15 KB at -Og
  config.max_open_sockets = 7;
  config.recv_wait_timeout = 5;
  config.lru_purge_enable = true;
  config.keep_alive_enable = true;
  config.keep_alive_idle = 5;
  config.keep_alive_interval = 5;
  config.keep_alive_count = 3;
  config.uri_match_fn = httpd_uri_match_wildcard;

  esp_err_t err = httpd_start(&g_server, &config);
  if (err != ESP_OK) {
    g_server = nullptr;
    return err;
  }

  const httpd_uri_t routes[] = {
      {.uri = "/*", .method = HTTP_GET, .handler = &wildcardHandler, .user_ctx = nullptr},
      {.uri = "/*", .method = HTTP_POST, .handler = &wildcardHandler, .user_ctx = nullptr},
      {.uri = "/*", .method = HTTP_PUT, .handler = &wildcardHandler, .user_ctx = nullptr},
      {.uri = "/*", .method = HTTP_DELETE, .handler = &wildcardHandler, .user_ctx = nullptr},
  };
  for (const auto& r : routes) {
    httpd_register_uri_handler(g_server, &r);
  }
  ADMIN_LOG_INFO("admin api started on port %u", static_cast<unsigned>(cfg.port));
  return ESP_OK;
}

bool adminApiRunning() { return g_server != nullptr; }

void setTimeSetter(TimeSetter setter) {
  std::lock_guard<std::mutex> lock(g_time_mutex);
  g_time_setter = std::move(setter);
}

void setDisplaySnapshotProvider(DisplaySnapshotProvider provider) {
  std::lock_guard<std::mutex> lock(g_snapshot_mutex);
  g_snapshot_provider = std::move(provider);
}

}  // namespace eego::admin
