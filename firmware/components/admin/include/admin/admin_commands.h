// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// AdminCommand —— httpd → app_task 的唯一命令通道（M12 定稿）。
//
// httpd handler 绝不直调 UI / SD 写 / 网络会话：配置持久化、恢复出厂、重启、
// 类型覆盖应用、WiFi 账号落 NVS 等全部经本队列在 app_task 上下文执行。
//
// 用法（M18 组合根）：
//   admin::setAdminCommandHandler(myHandler, ctx);   // 或 defaultCommandHandler
//   // app_task 主循环：
//   admin::serviceCommands(pdMS_TO_TICKS(50));       // 阻塞等命令→在己上下文执行
//
// httpd 侧：admin::enqueueAdminCommand(cmd, res, wait_ms) 入队并阻塞等结果。

#include <cstdint>
#include <memory>
#include <string>

#include "alert_core/json.h"
#include "admin/dashboard_config.h"
#include "admin/input_binding_model.h"

namespace eego::admin {

enum class AdminCommandType : uint8_t {
  ApplyConfig = 0,  // 校验后的合并 DOM：原子持久化 + 热生效（sources/overrides/admin 域）
  ApplyWifi,        // WiFi 账号变更（NVS 权威 + config wifi_list 同步 + 重连）
  ApplySettings,    // typed patch → app_task 对最新快照合并 + NVS commit
  WifiReset,        // 仅清 WiFi 凭据并打开限时恢复 AP（保留管理密码和偏好）
  FactoryReset,     // 清 sec/wifi/ui/ble/nimble_bond/device + 指定 SD 配置 → 重启
  Reboot,           // delay_ms 后 esp_restart
  ApplyDashboard,   // 完整 dashboard bundle：校验 + revision 检查 + 原子持久化
  DeleteDashboard,  // 删除 dashboard；同样支持 revision 冲突检查
  ApplyInputBindings,  // 稀疏输入覆盖或本地/远程 reset；app_task 重验并原子写 NVS
  BleClearBonds,       // 停止 BLE 角色并清 NimBLE bond/NVS；不开放远程配网
};

struct AdminCommand {
  AdminCommandType type = AdminCommandType::Reboot;

  // ApplyConfig：校验合并后的完整 DOM。
  alert_core::JsonValue config;
  // ApplyConfig：原始 admin.display patch（仅显示节；用于精确能力校验与迁移）。
  alert_core::JsonValue display_patch;

  // ApplySettings：httpd 已预校验的 partial typed patch；执行时基于最新快照重验。
  alert_core::JsonValue settings_patch;

  // ApplyConfig：变更的顶层 section 位掩码（httpd 按 PUT body 出现情况设置），
  // 供处理器避免不必要的全量热生效。
  static constexpr uint32_t kChangedAdmin = 1u << 0;
  static constexpr uint32_t kChangedSources = 1u << 1;
  static constexpr uint32_t kChangedOverrides = 1u << 2;
  static constexpr uint32_t kChangedMuteRules = 1u << 3;
  static constexpr uint32_t kChangedAdminDisplay = 1u << 4;
  uint32_t changed_sections = 0;

  // ApplyDashboard：不可变候选配置，默认处理器在 app_task 一次性提交。
  std::shared_ptr<const DashboardDocument> dashboard;
  bool dashboard_check_revision = false;
  uint64_t dashboard_expected_revision = 0;

  // ApplyInputBindings：revision 为期望值；reset 时忽略 bindings 并写入空覆盖表。
  std::shared_ptr<const InputBindingSet> input_bindings;
  bool input_reset = false;

  // ApplyWifi：待写入/删除的账号（ssid + 明文密码，仅命令内存生命周期，
  // 绝不落日志/SD）；remove=true 时 password 忽略。
  std::string wifi_ssid;
  std::string wifi_password;
  bool wifi_remove = false;

  // Reboot：重启前延迟（毫秒，M13：≥2s 让 202 响应发出）。
  uint32_t delay_ms = 2000;
};

struct AdminResult {
  bool ok = false;
  bool conflict = false;  // dashboard/input revision 冲突；httpd 映射 409
  std::string err;        // 安全描述（不含 secrets）
};

// 命令处理器：在 app_task 上下文执行（serviceCommands 调用栈）。
using AdminCommandHandler = void (*)(const AdminCommand& cmd, AdminResult& res, void* ctx);

// 注册处理器（app_task 组合根；通常用 defaultCommandHandler 或包装它）。
void setAdminCommandHandler(AdminCommandHandler handler, void* ctx);

// app_task 主循环调用：阻塞等待至多 wait_ms，收到命令则在调用方上下文
// 执行注册处理器并回传结果。返回 true = 执行了一条命令。
bool serviceCommands(uint32_t wait_ms);

// httpd 上下文调用：入队并阻塞等结果（至多 wait_ms）。
// false = 队列满/无处理器/等待超时（httpd 映射 503 unavailable）。
bool enqueueAdminCommand(const AdminCommand& cmd, AdminResult& res, uint32_t wait_ms);

}  // namespace eego::admin
