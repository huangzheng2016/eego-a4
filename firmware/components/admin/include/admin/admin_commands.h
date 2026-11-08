// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

//
//
//

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "alert_core/json.h"
#include "admin/dashboard_config.h"
#include "admin/input_binding_model.h"
#include "net_stack/wifi_list.h"

namespace eego::admin {
enum class AdminCommandType : uint8_t {
  ApplyConfig = 0,
  ApplyWifi,
  ApplySettings,
  WifiReset,
  FactoryReset,
  Reboot,
  ApplyDashboard,
  DeleteDashboard,
  ApplyInputBindings,
  BleClearBonds,
  WriteSourceCredential,
  DeleteSourceCredential,
  RenderPreview,
  // Runs `deferred` on app_task; used to move work out of small-stack contexts (esp_timer).
  RunDeferred,
};

struct RenderPreviewJob;

struct AdminCommand {
  AdminCommandType type = AdminCommandType::Reboot;

  alert_core::JsonValue config;

  alert_core::JsonValue settings_patch;

  static constexpr uint32_t kChangedAdmin = 1u << 0;
  static constexpr uint32_t kChangedOverrides = 1u << 2;
  static constexpr uint32_t kChangedMuteRules = 1u << 3;
  uint32_t changed_sections = 0;

  std::shared_ptr<const DashboardDocument> dashboard;
  bool dashboard_check_revision = false;
  uint64_t dashboard_expected_revision = 0;

  std::shared_ptr<const InputBindingSet> input_bindings;
  bool input_reset = false;

  std::string wifi_ssid;
  std::string wifi_password;
  bool wifi_remove = false;
  std::shared_ptr<const std::vector<net::WifiListEntry>> wifi_list;

  std::string credential_key;
  std::string credential_value;

  std::shared_ptr<RenderPreviewJob> preview;

  uint32_t delay_ms = 2000;

  void (*deferred)() = nullptr;
};

struct AdminResult {
  bool ok = false;
  bool conflict = false;
  std::string err;
};

using AdminCommandHandler = void (*)(const AdminCommand& cmd, AdminResult& res, void* ctx);

void setAdminCommandHandler(AdminCommandHandler handler, void* ctx);

bool serviceCommands(uint32_t wait_ms);

bool enqueueAdminCommand(const AdminCommand& cmd, AdminResult& res, uint32_t wait_ms);

// Fire-and-forget: queues `fn` for app_task without waiting. Returns false when the queue
// is unavailable or full.
bool postDeferred(void (*fn)());

}  // namespace eego::admin
