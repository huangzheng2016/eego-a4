// SPDX-License-Identifier: GPL-3.0-or-later
#include "source_secret_migration.h"

#include <cstdint>
#include <cstdio>

#include "net_stack/credentials.h"
#include "nvs.h"

namespace eego::admin::internal {
namespace {

std::string lower(std::string value) {
  for (char& c : value) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return value;
}

uint64_t hash(const std::string& text) {
  uint64_t value = 1469598103934665603ULL;
  for (const unsigned char c : text) {
    value ^= c;
    value *= 1099511628211ULL;
  }
  return value;
}

std::string keyFor(const std::string& identity, size_t probe) {
  const uint64_t digest = hash(identity + "#" + std::to_string(probe)) & 0xfffffffffffULL;
  char key[16];
  std::snprintf(key, sizeof(key), "mig_%011llx", static_cast<unsigned long long>(digest));
  return key;
}

}  // namespace

SourceSecretMigration::~SourceSecretMigration() { finish(); }

std::string SourceSecretMigration::add(const std::string& context, const std::string& header,
                                       const std::string& value, std::string& error) {
  if (!net::CredentialStore::validSourceValue(value)) {
    error = "legacy sensitive header is empty, too long, or contains control characters";
    return {};
  }
  const std::string identity = context + "\n" + lower(header);
  for (const auto& pending : pending_) {
    if (pending.identity == identity) {
      if (pending.value != value) {
        error = "legacy dashboard source and plan contain different secrets for one header";
        return {};
      }
      return net::CredentialStore::sourceRef(pending.key);
    }
  }
  for (size_t probe = 0; probe < 64; ++probe) {
    const std::string key = keyFor(identity, probe);
    bool pending_collision = false;
    for (const auto& pending : pending_) {
      if (pending.key == key) pending_collision = true;
    }
    if (pending_collision) continue;
    std::string existing;
    const esp_err_t read = net::CredentialStore::resolveSourceCredential(
        net::CredentialStore::sourceRef(key), existing);
    if (read == ESP_OK) {
      const bool same = existing == value;
      net::CredentialStore::secureErase(existing);
      if (same) return net::CredentialStore::sourceRef(key);
    } else if (read == ESP_ERR_NVS_NOT_FOUND) {
      pending_.push_back({identity, key, value, false});
      return net::CredentialStore::sourceRef(key);
    } else {
      net::CredentialStore::secureErase(existing);
      error = "cannot inspect source credential store during migration";
      return {};
    }
  }
  error = "cannot allocate a migrated source credential key";
  return {};
}

bool SourceSecretMigration::persist(std::string& error) {
  for (auto& pending : pending_) {
    if (pending.saved) continue;
    const esp_err_t err = net::CredentialStore::saveSourceCredential(pending.key, pending.value);
    if (err != ESP_OK) {
      error = "failed to persist migrated source credentials; configuration left unchanged";
      rollback();
      return false;
    }
    pending.saved = true;
  }
  return true;
}

void SourceSecretMigration::rollback() {
  for (auto& pending : pending_) {
    if (pending.saved) {
      net::CredentialStore::deleteSourceCredential(pending.key);
      pending.saved = false;
    }
  }
}

void SourceSecretMigration::finish() {
  for (auto& pending : pending_) net::CredentialStore::secureErase(pending.value);
  pending_.clear();
}

}  // namespace eego::admin::internal
