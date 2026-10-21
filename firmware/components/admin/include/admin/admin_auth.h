// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// 认证与限速 —— Bearer 会话、X-Ingest-Key、PBKDF2 管理密码、登录/端点限速
// （M13 T1/T3/T10、F4/F5 定稿）。
//
// secrets 边界：
//   * 管理密码仅存 PBKDF2-HMAC-SHA256(salt 16B esp_random, iter 100k) 到
//     NVS 命名空间 sec（key pw_hash）；任何响应/日志不输出 hash 或密码；
//   * 会话令牌 RAM-only（≤3 LRU 驱逐、TTL 默认 86400s 固定、重启全失效、
//     改密/恢复出厂全部吊销）；token = base64url(esp_random 32) 43 字符；
//   * X-Ingest-Key 存 NVS sec.ingest_key，可轮换（仅轮换响应完整显示一次，
//     平时只显示前 4 位掩码）；
//   * 比较一律常数时间（memcmp 循环/XOR 累积），401 不区分失败原因。
//
// 线程安全：httpd 任务与 app_task（命令执行）都会调用，内部互斥锁保护。

#include <cstdint>
#include <string>

#include "esp_err.h"

namespace eego::admin {

class AuthManager {
 public:
  static AuthManager& instance();

  // 确保 NVS 可用并加载 ingest key 缓存（幂等）。start/门户开启前调用。
  esp_err_t init();

  // ---- 管理密码 ----
  bool hasPassword();
  // 常数时间比对。内部计算一次 PBKDF2（S3 约百毫秒级，httpd 上下文可接受）。
  bool verifyPassword(const std::string& password);
  // 校验策略（≥8 位且非纯空白）→ 生成 salt → PBKDF2 → 写 NVS → 吊销全部会话。
  esp_err_t setPassword(const std::string& password);
  static bool passwordPolicyOk(const std::string& password);

  // ---- Bearer 会话 ----
  std::string createSession();                  // 43 字符 token；满 3 个 LRU 驱逐
  bool verifyToken(const std::string& token);   // 存在且未过期（滑动 last_used）
  void revokeSession(const std::string& token);
  void revokeAllSessions();
  void setSessionTtl(uint32_t ttl_s);
  uint32_t sessionTtl() const { return session_ttl_s_; }
  size_t sessionCount() const;

  // ---- X-Ingest-Key ----
  bool hasIngestKey();
  std::string rotateIngestKey();               // 新 key 写 NVS，旧 key 立即失效
  bool verifyIngestKey(const std::string& key);  // 常数时间
  std::string maskedIngestKey();               // 前 4 位 + "***"（未设置返回 ""）

  // ---- 登录限速（M13 T3）----
  // 每 IP 失败 login_lock_threshold 次锁 300s；全局 10 次/分钟滑动窗口。
  // false 时 retry_after_s 为建议 Retry-After。
  bool loginAllow(const std::string& ip, uint32_t& retry_after_s);
  void noteLoginSuccess(const std::string& ip);
  void noteLoginFailure(const std::string& ip);
  void setLockThreshold(uint32_t fails) { lock_threshold_ = fails ? fails : 5; }

  // ---- 通用固定窗口限速 ----
  // key 为任意限速维度（"get:<ip>" / "alerts" / "test:<token前缀>"）。
  bool consumeBudget(const std::string& key, uint32_t window_s, uint32_t limit,
                     uint32_t& retry_after_s);

 private:
  AuthManager() = default;

  mutable void* mutex_ = nullptr;  // SemaphoreHandle_t（延迟创建）
  uint32_t session_ttl_s_ = 86400;
  uint32_t lock_threshold_ = 5;
  std::string ingest_key_cache_;   // 内存缓存，避免每次读 NVS
};

}  // namespace eego::admin
