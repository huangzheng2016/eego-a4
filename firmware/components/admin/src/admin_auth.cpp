// SPDX-License-Identifier: GPL-3.0-or-later
// 认证与限速实现 —— 见 include/admin/admin_auth.h。
#include "admin/admin_auth.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "admin/admin_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "net_stack/credentials.h"

namespace eego::admin {

namespace {

constexpr uint32_t kPbkdf2Iterations = 100000;  // M13 F4：S3 单次约百毫秒级
constexpr size_t kSaltBytes = 16;
constexpr size_t kHashBytes = 32;
constexpr size_t kTokenBytes = 32;
constexpr size_t kMaxSessions = 3;  // M13 F5：≤3 LRU 驱逐
constexpr uint32_t kLoginLockMs = 300000;         // 5 分钟
constexpr uint32_t kLoginGlobalWindowMs = 60000;  // 全局 10 次/分钟
constexpr uint32_t kLoginGlobalLimit = 10;
constexpr size_t kMaxLoginTracks = 16;
constexpr size_t kMaxRateBuckets = 24;

const char* kNvsPwHashRef = "nvs:pw_hash";
const char kNvsSecNs[] = "sec";
const char kNvsIngestKey[] = "ingest_key";

int64_t nowMs() { return esp_timer_get_time() / 1000; }

// ---- RAM-only 会话/限速存储（文件作用域静态：重启即清，天然 RAM-only）----
struct SessionRec {
  std::string token;
  int64_t created_ms = 0;
  int64_t last_used_ms = 0;
};
std::vector<SessionRec> g_sessions;

struct LoginTrack {
  std::string ip;
  uint32_t fails = 0;
  int64_t locked_until_ms = 0;
};
std::vector<LoginTrack> g_login_tracks;

struct RateBucket {
  std::string key;
  int64_t window_start_ms = 0;
  uint32_t count = 0;
};
std::vector<RateBucket> g_rate_buckets;
RateBucket g_login_global{"", 0, 0};

// base64url（无填充）编码 —— token/key 的线格式（M13 F5：43 字符）。
std::string base64UrlEncode(const uint8_t* data, size_t len) {
  static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  out.reserve((len * 4 + 2) / 3);
  size_t i = 0;
  while (i + 2 < len) {
    uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
    out += tbl[(v >> 18) & 63];
    out += tbl[(v >> 12) & 63];
    out += tbl[(v >> 6) & 63];
    out += tbl[v & 63];
    i += 3;
  }
  if (i + 1 == len) {
    uint32_t v = uint32_t(data[i]) << 16;
    out += tbl[(v >> 18) & 63];
    out += tbl[(v >> 12) & 63];
  } else if (i + 2 == len) {
    uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
    out += tbl[(v >> 18) & 63];
    out += tbl[(v >> 12) & 63];
    out += tbl[(v >> 6) & 63];
  }
  return out;
}

std::string hexEncode(const uint8_t* data, size_t len) {
  static const char hex[] = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    out += hex[data[i] >> 4];
    out += hex[data[i] & 0xF];
  }
  return out;
}

bool hexDecode(const std::string& s, std::vector<uint8_t>& out) {
  if (s.size() % 2 != 0) return false;
  out.clear();
  out.reserve(s.size() / 2);
  auto val = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i + 1 < s.size(); i += 2) {
    int hi = val(s[i]), lo = val(s[i + 1]);
    if (hi < 0 || lo < 0) return false;
    out.push_back(uint8_t((hi << 4) | lo));
  }
  return true;
}

// 常数时间比较（长度不等也做满轮 XOR）。
bool constTimeEqual(const std::string& a, const std::string& b) {
  volatile uint8_t diff = static_cast<uint8_t>(a.size() ^ b.size());
  const size_t n = (a.size() > b.size()) ? a.size() : b.size();
  for (size_t i = 0; i < n; i++) {
    uint8_t ca = i < a.size() ? static_cast<uint8_t>(a[i]) : 0;
    uint8_t cb = i < b.size() ? static_cast<uint8_t>(b[i]) : 0;
    diff |= ca ^ cb;
  }
  return diff == 0;
}

// PBKDF2-HMAC-SHA256 单次导出。
bool pbkdf2Sha256(const std::string& password, const uint8_t* salt, size_t salt_len,
                  uint32_t iterations, uint8_t out[kHashBytes]) {
  const int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(
      MBEDTLS_MD_SHA256, reinterpret_cast<const unsigned char*>(password.data()),
      password.size(), salt, salt_len, iterations, kHashBytes, out);
  return rc == 0;
}

// NVS 存储格式："v1$<iter>$<hex(salt||hash)>"
std::string encodePwRecord(const uint8_t* salt, const uint8_t* hash) {
  std::vector<uint8_t> blob;
  blob.reserve(kSaltBytes + kHashBytes);
  blob.insert(blob.end(), salt, salt + kSaltBytes);
  blob.insert(blob.end(), hash, hash + kHashBytes);
  char head[32];
  snprintf(head, sizeof(head), "v1$%lu$", static_cast<unsigned long>(kPbkdf2Iterations));
  return std::string(head) + hexEncode(blob.data(), blob.size());
}

}  // namespace

// ===========================================================================
// AuthManager
// ===========================================================================

AuthManager& AuthManager::instance() {
  static AuthManager inst;
  return inst;
}

esp_err_t AuthManager::init() {
  if (mutex_ == nullptr) mutex_ = xSemaphoreCreateMutex();
  esp_err_t err = net::CredentialStore::ensureNvs();
  if (err != ESP_OK) return err;
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  if (ingest_key_cache_.empty()) {
    ingest_key_cache_ = net::CredentialStore::resolveToken("nvs:ingest_key");
  }
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  return ESP_OK;
}

// ---- 管理密码 ----

bool AuthManager::passwordPolicyOk(const std::string& password) {
  if (password.size() < 8 || password.size() > 128) return false;
  for (unsigned char c : password) {
    if (c > 0x20) return true;  // 存在非空白字符
  }
  return false;
}

bool AuthManager::hasPassword() {
  if (mutex_ == nullptr) return false;
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  const bool has = !net::CredentialStore::resolveToken(kNvsPwHashRef).empty();
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  return has;
}

bool AuthManager::verifyPassword(const std::string& password) {
  if (mutex_ == nullptr) return false;
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  const std::string rec = net::CredentialStore::resolveToken(kNvsPwHashRef);
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  if (rec.empty()) return false;

  const size_t p1 = rec.find('$');
  const size_t p2 = (p1 == std::string::npos) ? p1 : rec.find('$', p1 + 1);
  if (p1 == std::string::npos || p2 == std::string::npos || rec.compare(0, p1, "v1") != 0) {
    return false;
  }
  const uint32_t iter = static_cast<uint32_t>(strtoul(rec.c_str() + p1 + 1, nullptr, 10));
  std::vector<uint8_t> blob;
  if (iter == 0 || !hexDecode(rec.substr(p2 + 1), blob) ||
      blob.size() != kSaltBytes + kHashBytes) {
    return false;
  }
  uint8_t hash[kHashBytes];
  if (!pbkdf2Sha256(password, blob.data(), kSaltBytes, iter, hash)) return false;
  volatile uint8_t diff = 0;
  for (size_t i = 0; i < kHashBytes; i++) diff |= hash[i] ^ blob[kSaltBytes + i];
  return diff == 0;
}

esp_err_t AuthManager::setPassword(const std::string& password) {
  if (!passwordPolicyOk(password)) return ESP_ERR_INVALID_ARG;
  uint8_t salt[kSaltBytes];
  uint8_t hash[kHashBytes];
  esp_fill_random(salt, sizeof(salt));
  if (!pbkdf2Sha256(password, salt, sizeof(salt), kPbkdf2Iterations, hash)) {
    return ESP_FAIL;
  }
  const std::string rec = encodePwRecord(salt, hash);
  esp_err_t err = net::CredentialStore::saveToken(kNvsSecNs, "pw_hash", rec);
  if (err != ESP_OK) return err;
  revokeAllSessions();  // 改密吊销全部令牌（M13 F5）
  return ESP_OK;
}

// ---- Bearer 会话 ----

std::string AuthManager::createSession() {
  if (mutex_ == nullptr) return "";
  uint8_t raw[kTokenBytes];
  esp_fill_random(raw, sizeof(raw));
  const std::string token = base64UrlEncode(raw, sizeof(raw));

  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  const int64_t now = nowMs();
  const int64_t ttl_ms = static_cast<int64_t>(session_ttl_s_) * 1000;
  g_sessions.erase(std::remove_if(g_sessions.begin(), g_sessions.end(),
                                  [&](const SessionRec& s) {
                                    return now - s.created_ms > ttl_ms;
                                  }),
                   g_sessions.end());
  if (g_sessions.size() >= kMaxSessions) {
    auto oldest = g_sessions.begin();
    for (auto it = g_sessions.begin(); it != g_sessions.end(); ++it) {
      if (it->last_used_ms < oldest->last_used_ms) oldest = it;
    }
    g_sessions.erase(oldest);  // LRU 驱逐
  }
  g_sessions.push_back({token, now, now});
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  return token;
}

bool AuthManager::verifyToken(const std::string& token) {
  if (mutex_ == nullptr || token.size() != 43) return false;
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  const int64_t now = nowMs();
  const int64_t ttl_ms = static_cast<int64_t>(session_ttl_s_) * 1000;
  bool ok = false;
  for (auto& s : g_sessions) {
    if (constTimeEqual(s.token, token)) {
      if (now - s.created_ms <= ttl_ms) {
        s.last_used_ms = now;
        ok = true;
      }
      break;
    }
  }
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  return ok;
}

void AuthManager::revokeSession(const std::string& token) {
  if (mutex_ == nullptr) return;
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  for (auto it = g_sessions.begin(); it != g_sessions.end(); ++it) {
    if (constTimeEqual(it->token, token)) {
      g_sessions.erase(it);
      break;
    }
  }
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
}

void AuthManager::revokeAllSessions() {
  if (mutex_ == nullptr) return;
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  g_sessions.clear();
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
}

void AuthManager::setSessionTtl(uint32_t ttl_s) {
  if (ttl_s < 300) ttl_s = 300;
  if (ttl_s > 86400) ttl_s = 86400;
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  session_ttl_s_ = ttl_s;
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
}

size_t AuthManager::sessionCount() const {
  if (mutex_ == nullptr) return 0;
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  const size_t n = g_sessions.size();
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  return n;
}

// ---- X-Ingest-Key ----

bool AuthManager::hasIngestKey() {
  if (mutex_ == nullptr) return false;
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  const bool has = !ingest_key_cache_.empty();
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  return has;
}

std::string AuthManager::rotateIngestKey() {
  if (mutex_ == nullptr) return "";
  uint8_t raw[kTokenBytes];
  esp_fill_random(raw, sizeof(raw));
  const std::string key = base64UrlEncode(raw, sizeof(raw));
  if (net::CredentialStore::saveToken(kNvsSecNs, kNvsIngestKey, key) != ESP_OK) {
    return "";
  }
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  ingest_key_cache_ = key;
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  return key;
}

bool AuthManager::verifyIngestKey(const std::string& key) {
  if (mutex_ == nullptr || key.size() != 43) return false;
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  const bool ok = !ingest_key_cache_.empty() && constTimeEqual(ingest_key_cache_, key);
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  return ok;
}

std::string AuthManager::maskedIngestKey() {
  if (mutex_ == nullptr) return "";
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  const std::string masked =
      ingest_key_cache_.empty() ? "" : AdminLog::maskSecret(ingest_key_cache_);
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  return masked;
}

// ---- 登录限速 ----

bool AuthManager::loginAllow(const std::string& ip, uint32_t& retry_after_s) {
  retry_after_s = 0;
  if (mutex_ == nullptr) return true;

  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  const int64_t now = nowMs();
  bool allow = true;

  // 全局固定窗：10 次/分钟
  if (now - g_login_global.window_start_ms > kLoginGlobalWindowMs) {
    g_login_global.window_start_ms = now;
    g_login_global.count = 0;
  }
  if (g_login_global.count >= kLoginGlobalLimit) {
    retry_after_s = static_cast<uint32_t>(
        (kLoginGlobalWindowMs - (now - g_login_global.window_start_ms) + 999) / 1000);
    allow = false;
  } else {
    g_login_global.count++;
  }

  // 每 IP 锁定
  if (allow) {
    for (auto& t : g_login_tracks) {
      if (t.ip == ip) {
        if (now < t.locked_until_ms) {
          retry_after_s = static_cast<uint32_t>((t.locked_until_ms - now + 999) / 1000);
          allow = false;
        }
        break;
      }
    }
  }

  // 有界化：淘汰无失败且未锁定记录
  if (g_login_tracks.size() > kMaxLoginTracks) {
    g_login_tracks.erase(std::remove_if(g_login_tracks.begin(), g_login_tracks.end(),
                                        [&](const LoginTrack& t) {
                                          return t.fails == 0 && now >= t.locked_until_ms;
                                        }),
                         g_login_tracks.end());
  }
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  return allow;
}

void AuthManager::noteLoginSuccess(const std::string& ip) {
  if (mutex_ == nullptr) return;
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  for (auto& t : g_login_tracks) {
    if (t.ip == ip) {
      t.fails = 0;
      t.locked_until_ms = 0;
      break;
    }
  }
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
}

void AuthManager::noteLoginFailure(const std::string& ip) {
  if (mutex_ == nullptr) return;
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  bool found = false;
  for (auto& t : g_login_tracks) {
    if (t.ip == ip) {
      t.fails++;
      found = true;
      if (t.fails >= lock_threshold_) {
        t.locked_until_ms = nowMs() + kLoginLockMs;
        t.fails = 0;
        ADMIN_LOG_WARN("login locked: ip=%s (threshold reached)", ip.c_str());
      }
      break;
    }
  }
  if (!found && g_login_tracks.size() < kMaxLoginTracks) {
    g_login_tracks.push_back({ip, 1, 0});
  }
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
}

// ---- 通用固定窗口限速 ----

bool AuthManager::consumeBudget(const std::string& key, uint32_t window_s, uint32_t limit,
                                uint32_t& retry_after_s) {
  retry_after_s = 0;
  if (mutex_ == nullptr) return true;
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  const int64_t now = nowMs();
  bool allow = true;

  RateBucket* hit = nullptr;
  for (auto& b : g_rate_buckets) {
    if (b.key == key) {
      hit = &b;
      break;
    }
  }
  if (hit == nullptr) {
    if (g_rate_buckets.size() >= kMaxRateBuckets) {
      auto oldest = g_rate_buckets.begin();
      for (auto it = g_rate_buckets.begin(); it != g_rate_buckets.end(); ++it) {
        if (it->window_start_ms < oldest->window_start_ms) oldest = it;
      }
      g_rate_buckets.erase(oldest);
    }
    g_rate_buckets.push_back({key, now, 0});
    hit = &g_rate_buckets.back();
  }
  const int64_t window_ms = static_cast<int64_t>(window_s) * 1000;
  if (now - hit->window_start_ms > window_ms) {
    hit->window_start_ms = now;
    hit->count = 0;
  }
  if (hit->count >= limit) {
    retry_after_s = static_cast<uint32_t>(
        (window_ms - (now - hit->window_start_ms) + 999) / 1000);
    allow = false;
  } else {
    hit->count++;
  }
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  return allow;
}

}  // namespace eego::admin
