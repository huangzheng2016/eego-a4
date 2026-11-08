// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

//
//
//

#include <string>
#include <vector>

#include "alert_core/json.h"
#include "alert_core/store.h"
#include "esp_err.h"

namespace eego::admin {
struct FieldError {
  std::string field;
  std::string message;
};
using FieldErrors = std::vector<FieldError>;

struct AdminDomainConfig {
  bool auto_ap_fallback = false;
  uint32_t session_ttl_s = 86400;
  uint32_t login_lock_threshold = 5;
  std::string font;
  std::string log_level = "INFO";
  bool sd_log = false;
};

class ConfigStore {
 public:
  static ConfigStore& instance();

  esp_err_t init(const std::string& path);
  bool valid() const;
  const std::string& path() const;

  alert_core::JsonValue maskedDom() const;

  alert_core::JsonValue rawDom() const;

  bool validateAndMerge(const std::string& body, alert_core::JsonValue& merged,
                        FieldErrors& errs) const;

  bool commit(const alert_core::JsonValue& merged, std::string& err);

  AdminDomainConfig admin() const;

  std::vector<alert_core::MuteRule> muteRules() const;

  static const char* schemaJson();

  bool setWifiListInDom(alert_core::JsonValue& dom,
                        const std::vector<std::pair<std::string, std::string>>& ssid_ref_list)
      const;
  bool setMuteRulesInDom(alert_core::JsonValue& dom,
                         const std::vector<alert_core::MuteRule>& rules) const;

 private:
  ConfigStore() = default;

  mutable void* mutex_ = nullptr;  // SemaphoreHandle_t
  std::string path_;
  alert_core::JsonValue dom_;
  AdminDomainConfig admin_;
  bool valid_ = true;
  bool loaded_ = false;
};

}  // namespace eego::admin
