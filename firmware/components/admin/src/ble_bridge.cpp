// SPDX-License-Identifier: GPL-3.0-or-later
#include "admin/ble_bridge.h"

#include <cstring>
#include <string>
#include <vector>

#include "admin/admin.h"
#include "admin/admin_auth.h"
#include "admin/admin_log.h"
#include "admin/admin_types.h"
#include "alert_core/json.h"
#include "ble/ble_manager.h"
#include "ble/tunnel_frame.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "net_stack/credentials.h"
#include "net_stack/wifi_list.h"

namespace eego::admin {

namespace {

using alert_core::JsonValue;

constexpr uint32_t kTaskStack = 6144;
constexpr UBaseType_t kTaskPriority = 4;  // 低于 httpd（5）
constexpr int kMinTimeoutMs = 5000;
constexpr int kMaxTimeoutMs = 60000;
constexpr int kDefaultTimeoutMs = 20000;
constexpr size_t kMaxPathBytes = 256;
constexpr uint32_t kSendTimeoutMs = 60000;

enum class MsgType : uint8_t { LinkUp, LinkDown, Request };

struct Msg {
  MsgType type = MsgType::Request;
  uint8_t id = 0;
  const uint8_t* data = nullptr;
  size_t len = 0;
  uint16_t head_len = 0;
};

QueueHandle_t g_queue = nullptr;
TaskHandle_t g_task = nullptr;
std::string g_token;
esp_timer_handle_t g_reboot_timer = nullptr;

// ---- BLE 主机任务回调：只入队 ----
void cbLinkUp(void*) {
  Msg m;
  m.type = MsgType::LinkUp;
  if (g_queue != nullptr) xQueueSend(g_queue, &m, 0);
}

void cbLinkDown(void*) {
  Msg m;
  m.type = MsgType::LinkDown;
  if (g_queue != nullptr) xQueueSend(g_queue, &m, 0);
}

void cbMessage(void*, uint8_t id, const uint8_t* msg, size_t len, uint16_t head_len) {
  Msg m;
  m.type = MsgType::Request;
  m.id = id;
  m.data = msg;
  m.len = len;
  m.head_len = head_len;
  if (g_queue == nullptr || xQueueSend(g_queue, &m, 0) != pdTRUE) {
    ble::BleManager::instance().releaseMessage();
  }
}

void wipe(std::string& s) { net::CredentialStore::secureErase(s); }

void mintToken() {
  if (!g_token.empty()) AuthManager::instance().revokeSession(g_token);
  wipe(g_token);
  g_token = AuthManager::instance().createSession();
}

void dropToken() {
  if (!g_token.empty()) AuthManager::instance().revokeSession(g_token);
  wipe(g_token);
}

void refreshInfo() {
  const esp_app_desc_t* app = esp_app_get_description();
  ble::BleManager::instance().setTunnelInfo(app != nullptr ? app->version : "unknown",
                                            AuthManager::instance().hasPassword(),
                                            adminApiRunning());
}

// ---- 响应 ----
void reply(uint8_t id, int status, const JsonValue::Object& headers, const uint8_t* body,
           size_t body_len) {
  const std::string head =
      JsonValue::makeObject({{"s", JsonValue::makeInt(status)},
                             {"h", JsonValue::makeObject(headers)}})
          .toString();
  const esp_err_t err = ble::BleManager::instance().sendMessage(
      id, reinterpret_cast<const uint8_t*>(head.data()), static_cast<uint16_t>(head.size()), body,
      body_len, kSendTimeoutMs);
  if (err != ESP_OK) ADMIN_LOG_WARN("ble bridge: response send failed (%s)", esp_err_to_name(err));
}

void replyJson(uint8_t id, int status, const std::string& json) {
  reply(id, status, {{"content-type", JsonValue::makeString("application/json")}},
        reinterpret_cast<const uint8_t*>(json.data()), json.size());
}

void replyError(uint8_t id, int status, const char* code, const std::string& message) {
  const std::string json =
      JsonValue::makeObject(
          {{"error", JsonValue::makeObject({{"code", JsonValue::makeString(code)},
                                            {"message", JsonValue::makeString(message)}})}})
          .toString();
  replyJson(id, status, json);
}

// ---- 请求头解析 ----
struct Request {
  std::string method;
  std::string path;
  std::string content_type;
  std::string if_match;
  std::string accept;
  int timeout_ms = kDefaultTimeoutMs;
  const uint8_t* body = nullptr;
  size_t body_len = 0;
};

bool parseRequest(const Msg& m, Request& out, std::string& error) {
  JsonValue root;
  alert_core::JsonParseError perr;
  const std::string_view head(reinterpret_cast<const char*>(m.data), m.head_len);
  if (!alert_core::jsonParse(head, root, perr) || root.type() != JsonValue::Type::Object) {
    error = "invalid request head";
    return false;
  }
  const JsonValue* method = root.find("m");
  const JsonValue* path = root.find("p");
  if (method == nullptr || method->type() != JsonValue::Type::String || path == nullptr ||
      path->type() != JsonValue::Type::String) {
    error = "m and p are required";
    return false;
  }
  out.method = method->stringValue();
  if (out.method != "GET" && out.method != "POST" && out.method != "PUT" && out.method != "DELETE") {
    error = "unsupported method";
    return false;
  }
  out.path = path->stringValue();
  if (out.path.empty() || out.path[0] != '/' || out.path.size() > kMaxPathBytes ||
      out.path.find_first_of("\r\n ") != std::string::npos ||
      out.path.find("..") != std::string::npos) {
    error = "invalid path";
    return false;
  }
  if (const JsonValue* headers = root.find("h"); headers != nullptr && headers->type() == JsonValue::Type::Object) {
    for (const auto& [key, value] : headers->objectItems()) {
      if (value.type() != JsonValue::Type::String) continue;
      const std::string& v = value.stringValue();
      if (v.size() > 128 || v.find_first_of("\r\n") != std::string::npos) continue;
      if (key == "content-type") out.content_type = v;
      else if (key == "if-match") out.if_match = v;
      else if (key == "accept") out.accept = v;
      // 其余（authorization、x-ingest-key、origin、host…）一律丢弃
    }
  }
  if (const JsonValue* t = root.find("t"); t != nullptr && t->type() == JsonValue::Type::Int) {
    int64_t v = t->intValue();
    if (v < kMinTimeoutMs) v = kMinTimeoutMs;
    if (v > kMaxTimeoutMs) v = kMaxTimeoutMs;
    out.timeout_ms = static_cast<int>(v);
  }
  out.body = m.data + m.head_len;
  out.body_len = m.len - m.head_len;
  return true;
}

// ---- 回环转发 ----
struct Collected {
  JsonValue::Object headers;
  uint8_t* buf = nullptr;
  size_t len = 0;
  size_t cap = 0;
  bool overflow = false;
};

bool keepHeader(const char* key) {
  static const char* kKeep[] = {"content-type", "retry-after", "www-authenticate", "etag"};
  for (const char* k : kKeep) {
    if (strcasecmp(key, k) == 0) return true;
  }
  return strncasecmp(key, "x-preview-", 10) == 0;
}

esp_err_t onHttpEvent(esp_http_client_event_t* evt) {
  if (evt->event_id != HTTP_EVENT_ON_HEADER || evt->user_data == nullptr) return ESP_OK;
  auto* c = static_cast<Collected*>(evt->user_data);
  if (evt->header_key == nullptr || evt->header_value == nullptr || !keepHeader(evt->header_key)) {
    return ESP_OK;
  }
  std::string key = evt->header_key;
  for (auto& ch : key) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
  c->headers.emplace_back(key, JsonValue::makeString(evt->header_value));
  return ESP_OK;
}

bool grow(Collected& c, size_t need) {
  if (need <= c.cap) return true;
  if (need > ble::tunnel::kMaxResponseBytes) return false;
  size_t cap = c.cap == 0 ? 4096 : c.cap * 2;
  while (cap < need) cap *= 2;
  if (cap > ble::tunnel::kMaxResponseBytes) cap = ble::tunnel::kMaxResponseBytes;
  void* p = heap_caps_realloc(c.buf, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p == nullptr) return false;
  c.buf = static_cast<uint8_t*>(p);
  c.cap = cap;
  return true;
}

// 返回 HTTP 状态码；<0 = 回环失败（未必已派发）。
int forwardOnce(const Request& r, Collected& out) {
  const std::string url = "http://127.0.0.1:80/api/v1" + r.path;
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.timeout_ms = r.timeout_ms;
  cfg.buffer_size = 2048;
  cfg.buffer_size_tx = 1024;
  cfg.keep_alive_enable = false;
  cfg.disable_auto_redirect = true;
  cfg.event_handler = &onHttpEvent;
  cfg.user_data = &out;
  if (r.method == "POST") cfg.method = HTTP_METHOD_POST;
  else if (r.method == "PUT") cfg.method = HTTP_METHOD_PUT;
  else if (r.method == "DELETE") cfg.method = HTTP_METHOD_DELETE;
  else cfg.method = HTTP_METHOD_GET;
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == nullptr) return -1;
  std::string auth = "Bearer " + g_token;
  esp_http_client_set_header(client, "Authorization", auth.c_str());
  wipe(auth);
  if (!r.content_type.empty()) esp_http_client_set_header(client, "Content-Type", r.content_type.c_str());
  if (!r.if_match.empty()) esp_http_client_set_header(client, "If-Match", r.if_match.c_str());
  if (!r.accept.empty()) esp_http_client_set_header(client, "Accept", r.accept.c_str());

  int status = -1;
  esp_err_t err = esp_http_client_open(client, static_cast<int>(r.body_len));
  if (err == ESP_OK) {
    size_t sent = 0;
    while (sent < r.body_len) {
      size_t n = r.body_len - sent;
      if (n > 4096) n = 4096;
      const int w = esp_http_client_write(client, reinterpret_cast<const char*>(r.body + sent),
                                          static_cast<int>(n));
      if (w <= 0) {
        err = ESP_FAIL;
        break;
      }
      sent += static_cast<size_t>(w);
    }
  }
  if (err == ESP_OK && esp_http_client_fetch_headers(client) >= 0) {
    status = esp_http_client_get_status_code(client);
    while (true) {
      if (!grow(out, out.len + 2048)) {
        out.overflow = true;
        break;
      }
      const int n = esp_http_client_read(client, reinterpret_cast<char*>(out.buf + out.len),
                                         static_cast<int>(out.cap - out.len));
      if (n < 0) {
        status = -1;
        break;
      }
      if (n == 0) break;
      out.len += static_cast<size_t>(n);
    }
  }
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return status;
}

void freeCollected(Collected& c) {
  if (c.buf != nullptr) heap_caps_free(c.buf);
  c = Collected{};
}

void handleForward(uint8_t id, const Request& r) {
  if (!adminApiRunning()) {
    replyError(id, 503, "ble_bridge_unavailable", "admin API is not running");
    return;
  }
  if (g_token.empty()) mintToken();
  Collected c;
  int status = forwardOnce(r, c);
  if (status == 401 && ble::BleManager::instance().linkAuthenticated()) {
    // 401 是派发前的认证失败（如改密码吊销了全部会话）：重签 token 重试一次是安全的。
    freeCollected(c);
    mintToken();
    status = forwardOnce(r, c);
  }
  if (c.overflow) {
    freeCollected(c);
    replyError(id, 413, errc::kPayloadTooLarge, "response exceeds BLE tunnel limit");
    return;
  }
  if (status < 0) {
    freeCollected(c);
    replyError(id, 502, "ble_bridge_failed", "loopback request failed");
    return;
  }
  reply(id, status, c.headers, c.buf, c.len);
  freeCollected(c);
}

// ---- 首次设置（无管理密码，配网门户期间）----
void rebootCallback(void*) { esp_restart(); }

void scheduleBridgeReboot() {
  if (g_reboot_timer == nullptr) {
    esp_timer_create_args_t args = {};
    args.callback = &rebootCallback;
    args.name = "ble_setup_reboot";
    if (esp_timer_create(&args, &g_reboot_timer) != ESP_OK) {
      esp_restart();
      return;
    }
  }
  esp_timer_start_once(g_reboot_timer, 2000 * 1000);
}

void handleSetup(uint8_t id, const Request& r) {
  if (r.method != "POST" || r.path != "/setup") {
    replyError(id, 409, "not_provisioned", "set an admin password first (POST /setup)");
    return;
  }
  JsonValue root;
  alert_core::JsonParseError perr;
  const std::string_view text(reinterpret_cast<const char*>(r.body), r.body_len);
  if (r.body_len > 4096 || !alert_core::jsonParse(text, root, perr) ||
      root.type() != JsonValue::Type::Object) {
    replyError(id, 400, errc::kInvalidRequest, "invalid JSON");
    return;
  }
  const JsonValue* pw = root.find("admin_password");
  if (pw == nullptr || pw->type() != JsonValue::Type::String ||
      !AuthManager::passwordPolicyOk(pw->stringValue())) {
    replyError(id, 400, errc::kValidationFailed, "admin_password must be 8-128 bytes");
    return;
  }
  std::vector<net::WifiAccount> fresh;
  if (const JsonValue* nets = root.find("networks"); nets != nullptr) {
    if (nets->type() != JsonValue::Type::Array || nets->arrayItems().size() > net::kMaxWifiNetworks) {
      replyError(id, 400, errc::kValidationFailed, "networks must be an array of at most 8");
      return;
    }
    for (const JsonValue& n : nets->arrayItems()) {
      const JsonValue* ssid = n.find("ssid");
      const JsonValue* pass = n.find("password");
      const std::string p = (pass != nullptr && pass->type() == JsonValue::Type::String)
                                ? pass->stringValue()
                                : std::string();
      if (ssid == nullptr || ssid->type() != JsonValue::Type::String || ssid->stringValue().empty() ||
          ssid->stringValue().size() > 32 || (!p.empty() && (p.size() < 8 || p.size() > 63))) {
        for (auto& a : fresh) wipe(a.password);
        replyError(id, 400, errc::kValidationFailed,
                   "each network needs ssid (1-32 bytes) and an empty or 8-63 byte password");
        return;
      }
      fresh.push_back({ssid->stringValue(), p});
    }
  }
  if (!fresh.empty()) {
    // 新网络按给定顺序排在最前，其余已保存网络保持原顺序，超出上限的丢弃。
    std::vector<net::WifiAccount> saved;
    net::CredentialStore::loadWifiAccounts(saved);
    std::vector<net::WifiAccount> ordered = fresh;
    for (auto& a : saved) {
      bool dup = false;
      for (const auto& f : fresh) dup = dup || f.ssid == a.ssid;
      if (!dup && ordered.size() < net::kMaxWifiNetworks) ordered.push_back(a);
      wipe(a.password);
    }
    const esp_err_t err = net::CredentialStore::saveWifiAccounts(ordered);
    for (auto& a : ordered) wipe(a.password);
    for (auto& a : fresh) wipe(a.password);
    if (err != ESP_OK) {
      replyError(id, 500, errc::kInternal, "WiFi credential save failed");
      return;
    }
  }
  if (AuthManager::instance().setPassword(pw->stringValue()) != ESP_OK) {
    replyError(id, 500, errc::kInternal, "admin password save failed");
    return;
  }
  if (!AuthManager::instance().hasIngestKey()) AuthManager::instance().rotateIngestKey();
  ADMIN_LOG_WARN("ble setup complete, rebooting");
  emitAdminEvent({AdminEventType::ProvisionSucceeded, "", 0});
  replyJson(id, 200, "{\"status\":\"ok\",\"rebooting\":true}");
  ble::BleManager::instance().sendEvent("{\"ev\":\"reboot\"}");
  scheduleBridgeReboot();
}

void handleRequest(const Msg& m) {
  Request r;
  std::string error;
  if (!parseRequest(m, r, error)) {
    replyError(m.id, 400, errc::kInvalidRequest, error);
    return;
  }
  if (!AuthManager::instance().hasPassword()) {
    handleSetup(m.id, r);
  } else {
    handleForward(m.id, r);
  }
}

void bridgeTask(void*) {
  while (true) {
    Msg m;
    if (xQueueReceive(g_queue, &m, pdMS_TO_TICKS(2000)) != pdTRUE) {
      refreshInfo();
      continue;
    }
    switch (m.type) {
      case MsgType::LinkUp:
        refreshInfo();
        if (AuthManager::instance().hasPassword()) mintToken();
        break;
      case MsgType::LinkDown:
        dropToken();
        break;
      case MsgType::Request:
        handleRequest(m);
        ble::BleManager::instance().releaseMessage();
        break;
    }
  }
}

}  // namespace

esp_err_t startBleBridge() {
  if (g_task != nullptr) return ESP_OK;
  if (!ble::BleManager::compiled()) return ESP_ERR_NOT_SUPPORTED;
  if (g_queue == nullptr) g_queue = xQueueCreate(4, sizeof(Msg));
  if (g_queue == nullptr) return ESP_ERR_NO_MEM;
  refreshInfo();
  ble::TunnelHandler handler;
  handler.onLinkUp = &cbLinkUp;
  handler.onLinkDown = &cbLinkDown;
  handler.onMessage = &cbMessage;
  ble::BleManager::instance().setTunnelHandler(handler);
  if (xTaskCreate(&bridgeTask, "ble_bridge", kTaskStack, nullptr, kTaskPriority, &g_task) != pdPASS) {
    g_task = nullptr;
    ble::BleManager::instance().setTunnelHandler({});
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void stopBleBridge() {
  ble::BleManager::instance().setTunnelHandler({});
  if (g_task != nullptr) {
    vTaskDelete(g_task);
    g_task = nullptr;
  }
  dropToken();
}

}  // namespace eego::admin
