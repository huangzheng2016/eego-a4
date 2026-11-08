// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

//
//

#include <cstdint>
#include <string>

#include "esp_err.h"

namespace eego::admin {
class AuthManager {
 public:
  static AuthManager& instance();

  esp_err_t init();

  bool hasPassword();
  bool verifyPassword(const std::string& password);
  esp_err_t setPassword(const std::string& password);
  static bool passwordPolicyOk(const std::string& password);

  std::string createSession();
  bool verifyToken(const std::string& token);
  void revokeSession(const std::string& token);
  void revokeAllSessions();
  void setSessionTtl(uint32_t ttl_s);
  uint32_t sessionTtl() const { return session_ttl_s_; }
  size_t sessionCount() const;

  // ---- X-Ingest-Key ----
  bool hasIngestKey();
  std::string rotateIngestKey();
  bool verifyIngestKey(const std::string& key);
  std::string maskedIngestKey();

  bool loginAllow(const std::string& ip, uint32_t& retry_after_s);
  void noteLoginSuccess(const std::string& ip);
  void noteLoginFailure(const std::string& ip);
  void setLockThreshold(uint32_t fails) { lock_threshold_ = fails ? fails : 5; }

  bool consumeBudget(const std::string& key, uint32_t window_s, uint32_t limit,
                     uint32_t& retry_after_s);

 private:
  AuthManager() = default;

  mutable void* mutex_ = nullptr;
  uint32_t session_ttl_s_ = 86400;
  uint32_t lock_threshold_ = 5;
  std::string ingest_key_cache_;
};

}  // namespace eego::admin
