// SPDX-License-Identifier: GPL-3.0-or-later
// 首次配网门户 + AP 兜底实现 —— 见 include/admin/provision_portal.h。
//
// 功能基底参考 eego-a4-weread provision_portal（GPL-3.0，仅作行为参考，
// 本文件为新实现）；按 M13 T8/F1/F2 加固：窗口化 10 分钟自关、强制管理密码、
// 成功写 NVS 后重启、AP 兜底手动触发且管理 API 仍认证。
#include "admin/provision_portal.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "admin/admin.h"
#include "admin_internal.h"
#include "admin/admin_auth.h"
#include "admin/admin_log.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#include "net_stack/credentials.h"
#include "net_stack/wifi_manager.h"
#include "portal_page.h"

namespace eego::admin {

namespace {

constexpr char kApSsid[] = "EEGO-A4-Setup";  // 固定 SSID，不含设备唯一标识（M13 F2）
constexpr uint32_t kWindowMs = 600000;       // 10 分钟窗口（M13 T8）
constexpr int64_t kConnectTimeoutUs = 15000000;  // STA 试连 15s 超时（M13 F1）
constexpr uint32_t kRebootDelayMs = 3000;    // 配网成功后给手机回显再重启

httpd_handle_t g_portal_server = nullptr;
int g_dns_sock = -1;
TaskHandle_t g_dns_task = nullptr;
volatile bool g_dns_running = false;

// 失败原因码 → 中文（M13 F1：复用参考实现原因码表的功能）
const char* failReasonText(uint8_t reason) {
  switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
      return "找不到该网络（SSID 错误或信号差）";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
      return "密码错误";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_NOT_AUTHED:
      return "认证失败";
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_ASSOC_EXPIRE:
      return "关联失败（AP 拒绝）";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
      return "握手超时";
    case WIFI_REASON_BEACON_TIMEOUT:
    case WIFI_REASON_AUTH_LEAVE:
      return "连接被断开（信号差）";
    case 0:
      return "连接超时";
    default:
      return "连接失败";
  }
}

// ---------------------------------------------------------------------------
// DNS 劫持（captive portal）：一切 A 查询应答 192.168.4.1
// ---------------------------------------------------------------------------
void dnsTask(void* arg) {
  (void)arg;
  uint8_t rx[512];
  uint8_t tx[512];
  ADMIN_LOG_INFO("dns hijack started (-> 192.168.4.1)");
  while (g_dns_running && g_dns_sock >= 0) {
    struct sockaddr_in src;
    socklen_t src_len = sizeof(src);
    const int len = recvfrom(g_dns_sock, rx, sizeof(rx), 0,
                             reinterpret_cast<struct sockaddr*>(&src), &src_len);
    if (len < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
      break;
    }
    if (len < 17) continue;  // DNS 查询最小长度
    if (len > static_cast<int>(sizeof(tx))) continue;

    memcpy(tx, rx, static_cast<size_t>(len));
    tx[2] = 0x81;  // QR=1, AA=1, RD=1
    tx[3] = 0x80;  // RA=1, RCODE=0
    tx[6] = 0; tx[7] = 1;    // ANCOUNT = 1
    tx[8] = 0; tx[9] = 0;    // NSCOUNT = 0
    tx[10] = 0; tx[11] = 0;  // ARCOUNT = 0

    // 定位 question 段结束（name + null + type(2) + class(2)）
    int q_end = 12;
    while (q_end < len && rx[q_end] != 0) q_end += (rx[q_end] + 1);
    q_end += 5;
    if (q_end > len) continue;

    int pos = q_end;
    if (pos + 16 > static_cast<int>(sizeof(tx))) continue;
    tx[pos++] = 0xC0; tx[pos++] = 0x0C;  // 域名指针 → question
    tx[pos++] = 0; tx[pos++] = 1;        // TYPE = A
    tx[pos++] = 0; tx[pos++] = 1;        // CLASS = IN
    tx[pos++] = 0; tx[pos++] = 0; tx[pos++] = 0; tx[pos++] = 60;  // TTL = 60
    tx[pos++] = 0; tx[pos++] = 4;        // RDLENGTH = 4
    tx[pos++] = 192; tx[pos++] = 168; tx[pos++] = 4; tx[pos++] = 1;  // 192.168.4.1
    sendto(g_dns_sock, tx, static_cast<size_t>(pos), 0,
           reinterpret_cast<struct sockaddr*>(&src), src_len);
  }
  ADMIN_LOG_INFO("dns hijack stopped");
  vTaskDelete(nullptr);
}

void startDnsHijack() {
  g_dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (g_dns_sock < 0) {
    ADMIN_LOG_ERROR("dns socket create failed");
    return;
  }
  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(53);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(g_dns_sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    ADMIN_LOG_ERROR("dns bind failed");
    close(g_dns_sock);
    g_dns_sock = -1;
    return;
  }
  struct timeval tv = {};
  tv.tv_sec = 1;  // 1s 接收超时，便于及时退出
  setsockopt(g_dns_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  g_dns_running = true;
  if (xTaskCreate(&dnsTask, "admin_dns", 4096, nullptr, 5, &g_dns_task) != pdPASS) {
    g_dns_running = false;
    close(g_dns_sock);
    g_dns_sock = -1;
  }
}

void stopDnsHijack() {
  g_dns_running = false;
  if (g_dns_sock >= 0) {
    shutdown(g_dns_sock, SHUT_RDWR);
    close(g_dns_sock);
    g_dns_sock = -1;
  }
  g_dns_task = nullptr;  // 任务在 1s 超时后自行退出
}

// ---------------------------------------------------------------------------
// 门户 httpd handlers（门户命名空间与管理 API 隔离，M13 §1）
// ---------------------------------------------------------------------------
esp_err_t p_root(httpd_req_t* req) {
  ProvisionPortal::instance().notePortalActivity();
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Content-Security-Policy", "default-src 'self'");
  httpd_resp_send(req, pages::kPortalHtml, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

esp_err_t p_js(httpd_req_t* req) {
  ProvisionPortal::instance().notePortalActivity();
  httpd_resp_set_type(req, "application/javascript; charset=utf-8");
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_send(req, pages::kPortalJs, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

esp_err_t p_css(httpd_req_t* req) {
  ProvisionPortal::instance().notePortalActivity();
  httpd_resp_set_type(req, "text/css; charset=utf-8");
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_send(req, pages::kPortalCss, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

esp_err_t p_scan(httpd_req_t* req) {
  ProvisionPortal& p = ProvisionPortal::instance();
  p.notePortalActivity();
  // /scan 限速 1 次/5 秒（M13 限流预算）
  uint32_t retry = 0;
  if (!AuthManager::instance().consumeBudget("scan", 5, 1, retry)) {
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_status(req, "429 Too Many Requests");
    httpd_resp_send(req, "{\"error\":\"too many requests\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  wifi_scan_config_t scan_cfg = {};
  scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK) {
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, "[]", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  uint16_t ap_num = 0;
  esp_wifi_scan_get_ap_num(&ap_num);
  wifi_ap_record_t* aps = nullptr;
  if (ap_num > 0) {
    aps = static_cast<wifi_ap_record_t*>(malloc(sizeof(wifi_ap_record_t) * ap_num));
    if (aps != nullptr) esp_wifi_scan_get_ap_records(&ap_num, aps);
  }

  // 同名 SSID 合并：只保留信号最强的一条
  using alert_core::JsonValue;
  JsonValue::Array arr;
  for (int i = 0; i < ap_num && aps != nullptr; i++) {
    if (aps[i].ssid[0] == '\0') continue;  // 跳过隐藏 SSID
    bool merged = false;
    for (auto& item : arr) {
      const JsonValue* s = item.find("ssid");
      if (s != nullptr && s->stringValue() == reinterpret_cast<const char*>(aps[i].ssid)) {
        JsonValue* rssi = const_cast<JsonValue*>(item.find("rssi"));
        if (rssi != nullptr && aps[i].rssi > rssi->intValue(-100)) {
          *rssi = JsonValue::makeInt(aps[i].rssi);
        }
        merged = true;
        break;
      }
    }
    if (merged) continue;
    arr.push_back(JsonValue::makeObject({
        {"ssid", JsonValue::makeString(reinterpret_cast<const char*>(aps[i].ssid))},
        {"rssi", JsonValue::makeInt(aps[i].rssi)},
        {"secure", JsonValue::makeBool(aps[i].authmode != WIFI_AUTH_OPEN)},
    }));
  }
  free(aps);
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  const std::string body = JsonValue::makeArray(std::move(arr)).toString();
  httpd_resp_send(req, body.c_str(), body.size());
  return ESP_OK;
}

esp_err_t p_connect(httpd_req_t* req) {
  ProvisionPortal& p = ProvisionPortal::instance();
  p.notePortalActivity();

  char cl_buf[16];
  if (httpd_req_get_hdr_value_str(req, "Content-Length", cl_buf, sizeof(cl_buf)) != ESP_OK ||
      strtol(cl_buf, nullptr, 10) > 1024) {
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_status(req, "413 Payload Too Large");
    httpd_resp_send(req, "{\"state\":\"fail\",\"reason\":\"请求过大\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  char buf[1024] = {};
  const int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (len <= 0) {
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, "{\"state\":\"fail\",\"reason\":\"未收到数据\"}",
                    HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  buf[len] = '\0';

  alert_core::JsonValue root;
  alert_core::JsonParseError perr;
  std::string ssid, pass, admin_pass;
  if (!alert_core::jsonParse(buf, root, perr) ||
      root.type() != alert_core::JsonValue::Type::Object) {
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, "{\"state\":\"fail\",\"reason\":\"请求格式错误\"}",
                    HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  const alert_core::JsonValue* v = root.find("ssid");
  if (v != nullptr && v->type() == alert_core::JsonValue::Type::String) ssid = v->stringValue();
  v = root.find("pass");
  if (v != nullptr && v->type() == alert_core::JsonValue::Type::String) pass = v->stringValue();
  v = root.find("admin_pass");
  if (v != nullptr && v->type() == alert_core::JsonValue::Type::String) {
    admin_pass = v->stringValue();
  }

  if (ssid.empty() || ssid.size() > 32 || pass.size() > 63) {
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, "{\"state\":\"fail\",\"reason\":\"网络名或密码长度非法\"}",
                    HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }
  // 强制管理密码（M13 F1：≥8 位，首次设置，无默认密码）
  if (!AuthManager::passwordPolicyOk(admin_pass)) {
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, "{\"state\":\"fail\",\"reason\":\"管理密码至少 8 位且不能全为空白\"}",
                    HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  // 暂存配网参数（成功回调在 sys_evt 上下文使用）
  p.prov_ssid_ = ssid;
  p.prov_wifi_pass_ = pass;
  p.prov_admin_pass_ = admin_pass;
  p.prov_reason_.clear();
  p.prov_ip_.clear();
  p.prov_state_ = ProvisionPortal::ProvState::Connecting;
  p.connect_start_us_ = esp_timer_get_time();

  wifi_config_t sta_cfg = {};
  strncpy(reinterpret_cast<char*>(sta_cfg.sta.ssid), ssid.c_str(),
          sizeof(sta_cfg.sta.ssid) - 1);
  strncpy(reinterpret_cast<char*>(sta_cfg.sta.password), pass.c_str(),
          sizeof(sta_cfg.sta.password) - 1);
  sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
  esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
  esp_wifi_disconnect();
  esp_wifi_connect();
  ADMIN_LOG_INFO("portal: connecting to %s", ssid.c_str());

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_send(req, "{\"state\":\"connecting\"}", HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

esp_err_t p_status(httpd_req_t* req) {
  ProvisionPortal& p = ProvisionPortal::instance();
  p.notePortalActivity();
  p.checkConnectTimeout();

  const char* st = "idle";
  switch (p.provState()) {
    case ProvisionPortal::ProvState::Connecting: st = "connecting"; break;
    case ProvisionPortal::ProvState::Ok: st = "ok"; break;
    case ProvisionPortal::ProvState::Fail: st = "fail"; break;
    default: st = "idle"; break;
  }
  using alert_core::JsonValue;
  const std::string body = JsonValue::makeObject({
                                     {"state", JsonValue::makeString(st)},
                                     {"reason", JsonValue::makeString(p.provReason())},
                                     {"ip", JsonValue::makeString(p.provIp())},
                                     {"window_remaining_s",
                                      JsonValue::makeInt(
                                          static_cast<int64_t>(p.windowRemainingMs() / 1000))},
                                 })
                                 .toString();
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_send(req, body.c_str(), body.size());
  return ESP_OK;
}

esp_err_t p_not_found(httpd_req_t* req) {
  // 任意路径 → 302 到门户首页，触发手机 captive portal 弹窗
  httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_send(req, "", 0);
  return ESP_OK;
}

// 窗口定时器回调（esp_timer 上下文）
void windowTimerCallback(void* arg) {
  ProvisionPortal& p = ProvisionPortal::instance();
  if (p.isOpen()) {
    ADMIN_LOG_WARN("provision window expired; portal closing");
    p.close("expired");
  } else if (p.isApFallbackActive()) {
    ADMIN_LOG_WARN("ap fallback window expired; AP closing");
    p.closeApFallback("expired");
  }
}

}  // namespace

}  // namespace eego::admin

namespace eego::admin {

ProvisionPortal& ProvisionPortal::instance() {
  static ProvisionPortal inst;
  return inst;
}

// ---------------------------------------------------------------------------
// AP 控制（门户与兜底共用）
// ---------------------------------------------------------------------------
esp_err_t ProvisionPortal::startAp() {
  // 确保 WiFi 驱动与 STA netif 就绪（幂等）
  esp_err_t err = net::WifiManager::instance().init();
  if (err != ESP_OK) return err;

  // 已联网时先断开 STA：带连接切 APSTA 会导致 AP beacon 异常（weread 实证）
  esp_wifi_disconnect();
  vTaskDelay(pdMS_TO_TICKS(200));

  if (!ap_netif_created_) {
    esp_netif_create_default_wifi_ap();
    ap_netif_created_ = true;
  }
  err = esp_wifi_set_mode(WIFI_MODE_APSTA);
  if (err != ESP_OK) {
    ADMIN_LOG_ERROR("portal: set APSTA mode failed");
    return err;
  }
  wifi_config_t ap_cfg = {};
  strncpy(reinterpret_cast<char*>(ap_cfg.ap.ssid), kApSsid, sizeof(ap_cfg.ap.ssid) - 1);
  ap_cfg.ap.ssid_len = static_cast<uint8_t>(strlen(kApSsid));
  ap_cfg.ap.channel = 1;
  ap_cfg.ap.authmode = WIFI_AUTH_OPEN;  // 开放热点（M13 待决策项建议形态）
  ap_cfg.ap.max_connection = 4;
  ap_cfg.ap.beacon_interval = 100;
  err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
  if (err != ESP_OK) {
    ADMIN_LOG_ERROR("portal: set AP config failed");
    return err;
  }
  esp_wifi_start();
  ADMIN_LOG_INFO("ap started: %s (open)", kApSsid);
  return ESP_OK;
}

void ProvisionPortal::stopAp() {
  esp_wifi_set_mode(WIFI_MODE_STA);
  ADMIN_LOG_INFO("ap stopped");
}

// ---------------------------------------------------------------------------
// 窗口定时器（门户=空闲重置；兜底=固定截止）
// ---------------------------------------------------------------------------
void ProvisionPortal::startWindowTimer(bool idle_reset) {
  if (window_timer_ == nullptr) {
    const esp_timer_create_args_t args = {
        .callback = &windowTimerCallback, .name = "admin_window"};
    if (esp_timer_create(&args, reinterpret_cast<esp_timer_handle_t*>(&window_timer_)) !=
        ESP_OK) {
      window_timer_ = nullptr;
      return;
    }
  }
  esp_timer_stop(static_cast<esp_timer_handle_t>(window_timer_));
  esp_timer_start_once(static_cast<esp_timer_handle_t>(window_timer_),
                       static_cast<int64_t>(kWindowMs) * 1000);
  window_deadline_ms_ = (esp_timer_get_time() / 1000) + kWindowMs;
  (void)idle_reset;
}

void ProvisionPortal::stopWindowTimer() {
  if (window_timer_ != nullptr) {
    esp_timer_stop(static_cast<esp_timer_handle_t>(window_timer_));
  }
}

uint32_t ProvisionPortal::windowRemainingMs() const {
  if (!portal_running_ && !fallback_running_) return 0;
  const int64_t now = esp_timer_get_time() / 1000;
  if (now >= window_deadline_ms_) return 0;
  return static_cast<uint32_t>(window_deadline_ms_ - now);
}

void ProvisionPortal::notePortalActivity() {
  // 门户窗口：任何门户请求重置 10 分钟空闲计时（M13 T8）
  if (portal_running_) startWindowTimer(true);
}

// ---------------------------------------------------------------------------
// 配网门户（INIT_PROVISION）
// ---------------------------------------------------------------------------
esp_err_t ProvisionPortal::open() {
  if (portal_running_) return ESP_ERR_INVALID_STATE;
  // 80 端口互斥（防御性检查；模式判定本就在 app_task 上下文）
  if (adminApiRunning()) return ESP_ERR_INVALID_STATE;

  esp_err_t err = AuthManager::instance().init();  // NVS 就绪（幂等）
  if (err != ESP_OK) return err;
  err = startAp();
  if (err != ESP_OK) return err;

  // 注册配网 STA 事件（与 WifiManager 并存，互不干扰）
  esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &wifiEventHandler,
                             this);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifiEventHandler, this);
  sta_handler_registered_ = true;

  prov_state_ = ProvState::Idle;
  prov_reason_.clear();
  prov_ip_.clear();
  prov_ssid_.clear();
  prov_wifi_pass_.clear();
  prov_admin_pass_.clear();
  connect_start_us_ = 0;

  startDnsHijack();

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_uri_handlers = 10;
  config.stack_size = 6144;
  config.max_open_sockets = 5;
  config.recv_wait_timeout = 5;
  config.lru_purge_enable = true;
  config.keep_alive_enable = false;
  config.uri_match_fn = httpd_uri_match_wildcard;
  err = httpd_start(&g_portal_server, &config);
  if (err != ESP_OK) {
    g_portal_server = nullptr;
    stopDnsHijack();
    if (sta_handler_registered_) {
      esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                   &wifiEventHandler);
      esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifiEventHandler);
      sta_handler_registered_ = false;
    }
    stopAp();
    return err;
  }

  const httpd_uri_t routes[] = {
      {.uri = "/", .method = HTTP_GET, .handler = &p_root},
      {.uri = "/portal.js", .method = HTTP_GET, .handler = &p_js},
      {.uri = "/portal.css", .method = HTTP_GET, .handler = &p_css},
      {.uri = "/scan", .method = HTTP_GET, .handler = &p_scan},
      {.uri = "/connect", .method = HTTP_POST, .handler = &p_connect},
      {.uri = "/status", .method = HTTP_GET, .handler = &p_status},
      {.uri = "/*", .method = HTTP_GET, .handler = &p_not_found},
  };
  for (const auto& r : routes) {
    httpd_register_uri_handler(g_portal_server, &r);
  }

  portal_running_ = true;
  startWindowTimer(true);
  ADMIN_LOG_WARN("provision portal opened (window 10min, ssid=%s)", kApSsid);
  emitAdminEvent({AdminEventType::PortalOpened, "", kWindowMs / 1000});
  return ESP_OK;
}

void ProvisionPortal::close(const char* reason) {
  if (!portal_running_) return;
  portal_running_ = false;
  stopWindowTimer();
  if (g_portal_server != nullptr) {
    httpd_stop(g_portal_server);
    g_portal_server = nullptr;
  }
  stopDnsHijack();
  if (sta_handler_registered_) {
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                 &wifiEventHandler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifiEventHandler);
    sta_handler_registered_ = false;
  }
  stopAp();
  prov_wifi_pass_.clear();   // 明文密码不滞留
  prov_admin_pass_.clear();
  ADMIN_LOG_WARN("provision portal closed (%s)", reason != nullptr ? reason : "");
  emitAdminEvent({AdminEventType::PortalClosed, reason != nullptr ? reason : "", 0});
}

// ---------------------------------------------------------------------------
// AP 兜底（AP_FALLBACK；手动触发，管理 API 仍认证）
// ---------------------------------------------------------------------------
esp_err_t ProvisionPortal::openApFallback() {
  if (fallback_running_ || portal_running_) return ESP_ERR_INVALID_STATE;
  esp_err_t err = startAp();
  if (err != ESP_OK) return err;
  fallback_running_ = true;
  startWindowTimer(false);  // 固定 10 分钟窗口，无活动重置（M13 F2）
  ADMIN_LOG_WARN("ap fallback opened (window 10min, ssid=%s)", kApSsid);
  emitAdminEvent({AdminEventType::ApFallbackOpened, "", kWindowMs / 1000});
  return ESP_OK;
}

void ProvisionPortal::closeApFallback(const char* reason) {
  if (!fallback_running_) return;
  fallback_running_ = false;
  stopWindowTimer();
  stopAp();
  ADMIN_LOG_WARN("ap fallback closed (%s)", reason != nullptr ? reason : "");
  emitAdminEvent({AdminEventType::ApFallbackClosed, reason != nullptr ? reason : "", 0});
}

// ---------------------------------------------------------------------------
// 配网状态机
// ---------------------------------------------------------------------------
void ProvisionPortal::checkConnectTimeout() {
  if (prov_state_ != ProvState::Connecting || connect_start_us_ == 0) return;
  if (esp_timer_get_time() - connect_start_us_ > kConnectTimeoutUs) {
    onConnectFailed("连接超时");
    esp_wifi_disconnect();
  }
}

void ProvisionPortal::onConnectFailed(const char* reason) {
  prov_state_ = ProvState::Fail;
  prov_reason_ = reason != nullptr ? reason : "连接失败";
  ADMIN_LOG_WARN("portal: connect failed (%s)", prov_reason_.c_str());
}

void ProvisionPortal::onConnectSucceeded(const std::string& ip) {
  // M13 F1：成功 → WiFi 凭据 + PBKDF2 hash + 生成 ingest key 写 NVS → 重启
  prov_ip_ = ip;
  prov_state_ = ProvState::Ok;
  ADMIN_LOG_INFO("portal: connect ok ssid=%s ip=%s", prov_ssid_.c_str(), ip.c_str());

  // WiFi 凭据（NVS wifi 命名空间；重新配网即替换旧账号，防残留失效网络）
  const std::vector<net::WifiAccount> accounts = {{prov_ssid_, prov_wifi_pass_}};
  if (net::CredentialStore::saveWifiAccounts(accounts) != ESP_OK) {
    ADMIN_LOG_ERROR("portal: wifi credential persist failed");
  }

  // 管理密码（PBKDF2 → NVS sec）
  if (!prov_admin_pass_.empty()) {
    if (AuthManager::instance().setPassword(prov_admin_pass_) != ESP_OK) {
      ADMIN_LOG_ERROR("portal: admin password persist failed");
    }
  }
  // 生成 ingest key（首次配网；已存在则保持——rotate 走管理 API）
  if (!AuthManager::instance().hasIngestKey()) {
    AuthManager::instance().rotateIngestKey();
  }
  prov_wifi_pass_.clear();   // 明文密码不滞留
  prov_admin_pass_.clear();

  emitAdminEvent({AdminEventType::ProvisionSucceeded, "", 0});
  // 延迟重启：给手机端 /status 回显成功的时间（响应已 flush 后设备重启）
  ADMIN_LOG_WARN("portal: provision complete, rebooting in %lums",
                 static_cast<unsigned long>(kRebootDelayMs));
  // 复用 app_task 的重启路径：直接经命令队列不行（可能无处理器），
  // 这里用一次性 esp_timer 重启（与 defaultCommandHandler 相同机制）。
  static esp_timer_handle_t reboot_timer = nullptr;
  if (reboot_timer == nullptr) {
    const esp_timer_create_args_t args = {
        .callback = [](void*) { esp_restart(); }, .name = "prov_reboot"};
    esp_timer_create(&args, &reboot_timer);
  }
  esp_timer_start_once(reboot_timer, static_cast<int64_t>(kRebootDelayMs) * 1000);
}

void ProvisionPortal::wifiEventHandler(void* arg, esp_event_base_t base, int32_t id,
                                       void* data) {
  ProvisionPortal* self = static_cast<ProvisionPortal*>(arg);
  if (self == nullptr) self = &ProvisionPortal::instance();
  if (!self->portal_running_) return;

  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    const auto* evt = static_cast<wifi_event_sta_disconnected_t*>(data);
    if (self->prov_state_ == ProvState::Connecting) {
      self->onConnectFailed(failReasonText(evt->reason));
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    const auto* evt = static_cast<ip_event_got_ip_t*>(data);
    if (self->prov_state_ == ProvState::Connecting) {
      char ip[16];
      snprintf(ip, sizeof(ip), IPSTR, IP2STR(&evt->ip_info.ip));
      self->onConnectSucceeded(ip);
    }
  }
}

}  // namespace eego::admin
