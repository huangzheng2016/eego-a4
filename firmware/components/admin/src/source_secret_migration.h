// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace eego::admin::internal {

class SourceSecretMigration {
 public:
  SourceSecretMigration() = default;
  SourceSecretMigration(const SourceSecretMigration&) = delete;
  SourceSecretMigration& operator=(const SourceSecretMigration&) = delete;
  ~SourceSecretMigration();

  std::string add(const std::string& context, const std::string& header,
                  const std::string& value, std::string& error);
  bool persist(std::string& error);
  void rollback();
  void finish();

 private:
  struct Pending {
    std::string identity;
    std::string key;
    std::string value;
    bool saved = false;
  };
  std::vector<Pending> pending_;
};

}  // namespace eego::admin::internal
