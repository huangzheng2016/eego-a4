// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <string>

namespace eego::admin::internal {

struct ConfigTransactionHooks {
  std::function<bool(std::string&)> commit_config;
  std::function<bool(std::string&)> apply_runtime;
  std::function<bool(std::string&)> rollback_config;
  std::function<bool(std::string&)> rollback_runtime;
};

// Pure coordination boundary, host-testable without ESP-IDF. Empty hooks are
// no-ops. Every failure path attempts the applicable rollback and reports any
// rollback error alongside the original failure.
bool runConfigTransaction(const ConfigTransactionHooks& hooks, std::string& error);

}  // namespace eego::admin::internal
