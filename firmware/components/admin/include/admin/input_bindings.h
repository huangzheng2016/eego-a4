// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

#include "admin/input_binding_model.h"
#include "esp_err.h"

namespace eego::admin {

class InputBindings {
 public:
  static InputBindings& instance();

  esp_err_t init();
  InputBindingSet snapshot() const;

  // Writers run in app_task. Validation, revision checking and one-record NVS
  // commit complete before the RAM snapshot is replaced.
  bool update(const std::vector<InputBinding>& candidate, bool check_revision,
              uint32_t expected_revision, const dashboard::Dashboard* dashboard,
              std::string& error, bool& conflict);
  bool reset(bool check_revision, uint32_t expected_revision,
             const dashboard::Dashboard* dashboard, std::string& error, bool& conflict);

  static esp_err_t erasePersisted();

 private:
  InputBindings() = default;
  void ensureMutex() const;

  mutable void* mutex_ = nullptr;
  InputBindingSet values_;
  bool initialized_ = false;
};

}  // namespace eego::admin
