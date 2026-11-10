// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

//
//   AuthManager::instance().init();
//   ConfigStore::instance().init("/sdcard/alertpanel/config.json");
//   AdminLog::instance().init(level);
//   DashboardStore::instance().init("/sdcard/alertpanel/dashboard.json");
//   admin::setAlertSnapshotSource(...);
//   admin::setAlertActionSink(...);
//   admin::setAdminCommandHandler(&admin::defaultCommandHandler, nullptr);
//   admin::setAdminEventCallback(onAdminEvent, &app);
//   admin::serviceCommands(pdMS_TO_TICKS(50));
//   if (!AuthManager::instance().hasPassword()) {
//     ProvisionPortal::instance().open();        // INIT_PROVISION
//   } else {
//     admin::startAdminApi(cfg);                 // ONLINE/OFFLINE
//   }
//

#include <cstdint>
#include <functional>
#include <string>

#include "admin/admin_auth.h"
#include "admin/admin_commands.h"
#include "admin/admin_config.h"
#include "admin/admin_log.h"
#include "admin/admin_types.h"
#include "admin/input_bindings.h"
#include "admin/provision_portal.h"
#include "alert_core/type_registry.h"
#include "esp_err.h"

namespace eego::admin {
struct AdminConfig {
  uint16_t port = 80;
  std::string hostname = "eego-a4";
  alert_core::TypeRegistry* registry = nullptr;
};

esp_err_t startAdminApi(const AdminConfig& cfg);
bool adminApiRunning();
int64_t lastAuthenticatedRequestMs();
// Same, but only requests that arrived over WiFi (not the BLE loopback bridge).
int64_t lastWifiAdminRequestMs();

struct TimeSetResult {
  bool applied = false;
  int64_t drift_s = 0;
  bool sntp_synced = false;
};
using TimeSetter = std::function<TimeSetResult(int64_t unix_ms)>;
void setTimeSetter(TimeSetter setter);
void setAlertSnapshotSource(AlertSnapshotSource src);
void setAlertActionSink(AlertActionSink sink);
void setAdminEventCallback(AdminEventCallback cb, void* ctx);

void defaultCommandHandler(const AdminCommand& cmd, AdminResult& res, void* ctx);

void emitAdminEvent(const AdminEvent& ev);

}  // namespace eego::admin
