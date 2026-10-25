// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_transaction.h"

namespace eego::admin::internal {

namespace {

using Step = std::function<bool(std::string&)>;

bool run(const Step& step, std::string& error) { return !step || step(error); }

void rollback(const Step& step, const char* label, std::string& error) {
  if (!step) return;
  std::string detail;
  if (!step(detail)) {
    error += "; ";
    error += label;
    error += " rollback failed";
    if (!detail.empty()) {
      error += ": ";
      error += detail;
    }
  }
}

}  // namespace

bool runConfigTransaction(const ConfigTransactionHooks& hooks, std::string& error) {
  if (!run(hooks.commit_config, error)) {
    if (error.empty()) error = "config commit failed";
    rollback(hooks.rollback_config, "config", error);
    return false;
  }
  if (!run(hooks.apply_runtime, error)) {
    if (error.empty()) error = "runtime apply failed";
    rollback(hooks.rollback_runtime, "runtime", error);
    rollback(hooks.rollback_config, "config", error);
    return false;
  }
  return true;
}

}  // namespace eego::admin::internal
