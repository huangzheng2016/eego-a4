// SPDX-License-Identifier: GPL-3.0-or-later
#include "source_credential_usage.h"

#include "admin/dashboard_config.h"
#include "net_stack/credentials.h"

namespace eego::admin::internal {
namespace {

bool sourceUsesCredential(const dashboard::Source& source, const std::string& ref) {
  if (source.auth_token_ref == ref) return true;
  for (const auto& header : source.header_secret_refs) {
    if (header.value == ref) return true;
  }
  return false;
}

}  // namespace

bool sourceCredentialInUse(const std::string& key) {
  const std::string ref = net::CredentialStore::sourceRef(key);
  if (ref.empty()) return false;
  const auto document = DashboardStore::instance().snapshot();
  if (document == nullptr) return false;
  for (const auto& source : document->dashboard.sources) {
    if (sourceUsesCredential(source, ref)) return true;
  }
  for (const auto& plan : document->plans) {
    if (sourceUsesCredential(plan.source, ref)) return true;
  }
  return false;
}

esp_err_t deleteSourceCredentialIfUnreferenced(const std::string& key, bool& in_use) {
  in_use = false;
  if (!net::CredentialStore::validSourceKeyForWrite(key)) return ESP_ERR_INVALID_ARG;
  in_use = sourceCredentialInUse(key);
  if (in_use) return ESP_ERR_INVALID_STATE;
  return net::CredentialStore::deleteSourceCredential(key);
}

}  // namespace eego::admin::internal
