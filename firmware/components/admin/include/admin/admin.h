// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// admin 组件公共入口 —— FW4 局域网管理与远程管理（M17）。
//
// 快速上手（M18 组合根）：
//   // 0. 一次性初始化（只在组合根调用一次；startAdminApi/门户不再重复 init）
//   AuthManager::instance().init();
//   ConfigStore::instance().init("/sdcard/alertpanel/config.json");
//   AdminLog::instance().init(level);
//   DashboardStore::instance().init("/sdcard/alertpanel/dashboard.json");
//   // 1. 接线（M20 提供 net_stack 侧实现后注入；未注入时读写端点回 503）
//   admin::setAlertSnapshotSource(...);
//   admin::setAlertActionSink(...);
//   admin::setAdminCommandHandler(&admin::defaultCommandHandler, nullptr);
//   admin::setAdminEventCallback(onAdminEvent, &app);
//   // 2. app_task 主循环
//   admin::serviceCommands(pdMS_TO_TICKS(50));
//   // 3. 模式判定（app_task 上下文，M13 bootstrap）：
//   if (!AuthManager::instance().hasPassword()) {
//     ProvisionPortal::instance().open();        // INIT_PROVISION
//   } else {
//     admin::startAdminApi(cfg);                 // ONLINE/OFFLINE
//   }
//   // AP 兜底：admin::ProvisionPortal::instance().openApFallback();
//
// 首版边界（M13 T7 定稿）：本组件不提供任何 /firmware/* 或远程 OTA 端点，
// 一切未知路径 404；固件刷写只走 USB（RECOVERY.md 工作流）。

#include <cstdint>
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
  uint16_t port = 80;                          // 管理端口（绑 0.0.0.0，STA/AP 均可达）
  std::string hostname = "eego-a4";            // Host/Origin 白名单补充（mDNS 名）
  alert_core::TypeRegistry* registry = nullptr;  // GET /types 数据源（写经 AdminCommand）
};

// ---- 管理 API 服务器（app_task 按设备模式启停；幂等）----
// 门户开启期间拒绝启动（80 端口互斥，防御性检查）。
esp_err_t startAdminApi(const AdminConfig& cfg);
bool adminApiRunning();

// ---- 依赖注入（M18 组合根；M20 提供 net_stack 实现后告警读写全通）----
void setAlertSnapshotSource(AlertSnapshotSource src);
void setAlertActionSink(AlertActionSink sink);
void setAdminEventCallback(AdminEventCallback cb, void* ctx);

// 默认命令处理器：ApplyConfig/ApplySettings（校验、事务回滚、订阅热生效）、
// ApplyDashboard/DeleteDashboard（完整 bundle 校验、revision 检查与原子持久化）、
// ApplyInputBindings（稀疏覆盖、白名单重验、revision 与单记录 NVS 原子写）、
// ApplyWifi、WifiReset（仅清 WiFi + 限时恢复 AP）、FactoryReset（清
// sec/wifi/ui/ble/device/dashboard/input + 指定 SD 配置，保留字体）、Reboot。仅在 app_task 执行。
void defaultCommandHandler(const AdminCommand& cmd, AdminResult& res, void* ctx);

// 组件内部共用：事件发射（portal/httpd → app_task 回调）。
void emitAdminEvent(const AdminEvent& ev);

}  // namespace eego::admin
