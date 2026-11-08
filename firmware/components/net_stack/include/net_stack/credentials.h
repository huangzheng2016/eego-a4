// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

//
//

#include <string>
#include <vector>

#include "esp_err.h"

namespace eego::net {
struct WifiAccount {
  std::string ssid;
  std::string password;
};

struct SourceCredentialMetadata {
  std::string key;
  std::string ref;
};

class CredentialStore {
 public:
  static constexpr const char* kSourceNamespace = "source";
  static constexpr size_t kSourceKeyMaxBytes = 15;
  static constexpr size_t kSourceValueMaxBytes = 1024;

  static esp_err_t ensureNvs();

  static size_t loadWifiAccounts(std::vector<WifiAccount>& out);
  static esp_err_t saveWifiAccounts(const std::vector<WifiAccount>& in);
  static esp_err_t clearWifiAccounts();

  static std::string resolveToken(const std::string& ref);
  static esp_err_t saveToken(const std::string& ns, const std::string& key,
                             const std::string& value);
  static esp_err_t clearNamespace(const std::string& ns);

  static bool validSourceKey(const std::string& key);
  static bool validSourceKeyForWrite(const std::string& key);
  static bool validSourceValue(const std::string& value);
  static bool validSourceRef(const std::string& ref);
  static std::string sourceRef(const std::string& key);
  static std::string sourceKey(const std::string& ref);
  static esp_err_t saveSourceCredential(const std::string& key, const std::string& value);
  static esp_err_t deleteSourceCredential(const std::string& key);
  static esp_err_t resolveSourceCredential(const std::string& ref, std::string& value);
  static esp_err_t listSourceCredentials(std::vector<SourceCredentialMetadata>& out);

  static bool validHeaderName(const std::string& name);
  static bool safeHeaderValue(const std::string& value);
  static bool sensitiveHeaderName(const std::string& name);
  static void secureErase(std::string& value);

};

}  // namespace eego::net
