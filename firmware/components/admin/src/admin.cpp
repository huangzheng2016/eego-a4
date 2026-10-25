// SPDX-License-Identifier: GPL-3.0-or-later
// admin 组件装配 —— 注入点存储、事件发射、默认命令处理器。
#include "admin/admin.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#include "admin_internal.h"
#include "admin/input_bindings.h"
#include "admin/render_preview.h"
#include "ble/ble_manager.h"
#include "config_transaction.h"
#include "device_settings/device_settings.h"
#include "esp_timer.h"
#include "net_stack/credentials.h"
#include "net_stack/net_task.h"
#include "net_stack/wifi_list.h"
#include "net_stack/wifi_manager.h"
#include "sd_card.h"
#include "source_credential_usage.h"

namespace eego::admin {

namespace {

AlertSnapshotSource g_snapshot_source;
AlertActionSink g_action_sink;
AdminEventCallback g_event_cb = nullptr;
void* g_event_ctx = nullptr;
alert_core::TypeRegistry* g_registry = nullptr;

// 延迟重启（esp_timer 一次性；≥2s 让 202/204 响应先 flush，M13 高危端点守卫）
esp_timer_handle_t g_reboot_timer = nullptr;

void rebootTimerCallback(void* arg) {
  (void)arg;
  esp_restart();
}

void scheduleReboot(uint32_t delay_ms) {
  if (delay_ms < 2000) delay_ms = 2000;  // M13：≥2s
  if (g_reboot_timer == nullptr) {
    const esp_timer_create_args_t args = {
        .callback = &rebootTimerCallback, .name = "admin_reboot"};
    if (esp_timer_create(&args, &g_reboot_timer) != ESP_OK) {
      esp_restart();  // 定时器都建不起来就直接重启
      return;
    }
  }
  esp_timer_stop(g_reboot_timer);
  esp_timer_start_once(g_reboot_timer, static_cast<int64_t>(delay_ms) * 1000);
}

// 只删除明确列出的配置/数据文件。不得递归删目录：字体和未知用户文件保留。
bool removeFactoryConfigs() {
  static constexpr const char* kPaths[] = {
      "/sdcard/alertpanel/config.json",
      "/sdcard/alertpanel/config.json.tmp",
      "/sdcard/alertpanel/config.json.bak",
      "/sdcard/alertpanel/config.json.corrupt",
      "/sdcard/alertpanel/alerts.db",
      "/sdcard/alertpanel/alerts.db.tmp",
      "/sdcard/alertpanel/alerts.db.bak",
      "/sdcard/alertpanel/dashboard.json",
      "/sdcard/alertpanel/dashboard.json.tmp",
      "/sdcard/alertpanel/dashboard.json.bak",
      "/sdcard/alertpanel/dashboard.json.corrupt",
  };
  bool ok = true;
  for (const char* path : kPaths) {
    if (remove(path) != 0 && errno != ENOENT) ok = false;
  }
  return ok;
}

bool syncMuteRules(const std::vector<alert_core::MuteRule>& rules, std::string& error) {
  net::NetStack& ns = net::NetStack::instance();
  if (!ns.running()) {
    error = "net_task is unavailable";
    return false;
  }
  return ns.setMuteRules(rules, error);
}

struct RuntimeConfig {
  AdminDomainConfig admin;
  alert_core::JsonValue overrides = alert_core::JsonValue::makeObject({});
  std::vector<alert_core::MuteRule> mute_rules;
};

RuntimeConfig captureRuntime(ConfigStore& store, const alert_core::JsonValue& dom) {
  RuntimeConfig out;
  out.admin = store.admin();
  if (const alert_core::JsonValue* overrides = dom.find("alert_type_overrides")) {
    out.overrides = *overrides;
  }
  out.mute_rules = store.muteRules();
  return out;
}

bool applyRuntime(const RuntimeConfig& config, uint32_t sections, std::string& error) {
  if ((sections & AdminCommand::kChangedAdmin) != 0) {
    AuthManager::instance().setSessionTtl(config.admin.session_ttl_s);
    AuthManager::instance().setLockThreshold(config.admin.login_lock_threshold);
    LogLevel level;
    if (logLevelFromString(config.admin.log_level, level)) {
      AdminLog::instance().setMinLevel(level);
    }
  }
  if ((sections & AdminCommand::kChangedOverrides) != 0) {
    if (g_registry == nullptr) {
      error = "alert type registry is unavailable";
      return false;
    }
    g_registry->clearOverrides();
    alert_core::ValidationError detail;
    if (!g_registry->applyOverridesJson(config.overrides, detail)) {
      error = "type overrides apply failed: " + detail.field + "." + detail.message;
      return false;
    }
  }
  if ((sections & AdminCommand::kChangedMuteRules) != 0 &&
      !syncMuteRules(config.mute_rules, error)) {
    return false;
  }
  return true;
}

}  // namespace

// ===========================================================================
// 注入点与事件
// ===========================================================================

void setAlertSnapshotSource(AlertSnapshotSource src) { g_snapshot_source = std::move(src); }
void setAlertActionSink(AlertActionSink sink) { g_action_sink = std::move(sink); }

void setAdminEventCallback(AdminEventCallback cb, void* ctx) {
  g_event_cb = cb;
  g_event_ctx = ctx;
}

void emitAdminEvent(const AdminEvent& ev) {
  if (g_event_cb != nullptr) g_event_cb(ev, g_event_ctx);
}

namespace internal {

const AlertSnapshotSource& snapshotSource() { return g_snapshot_source; }
const AlertActionSink& actionSink() { return g_action_sink; }
AdminEventCallback eventCallback() { return g_event_cb; }
void* eventCtx() { return g_event_ctx; }
alert_core::TypeRegistry* registry() { return g_registry; }
void setRegistry(alert_core::TypeRegistry* reg) { g_registry = reg; }

}  // namespace internal

// ===========================================================================
// 默认命令处理器（app_task 上下文执行）
// ===========================================================================

void defaultCommandHandler(const AdminCommand& cmd, AdminResult& res, void* ctx) {
  (void)ctx;
  ConfigStore& store = ConfigStore::instance();

  switch (cmd.type) {
    case AdminCommandType::ApplyConfig: {
      if ((cmd.changed_sections & AdminCommand::kChangedOverrides) != 0 && g_registry == nullptr) {
        res.ok = false;
        res.err = "alert type registry is unavailable";
        return;
      }
      if ((cmd.changed_sections & AdminCommand::kChangedMuteRules) != 0) {
        if (!net::NetStack::instance().running()) {
          res.ok = false;
          res.err = "net_task is unavailable; config was not committed";
          return;
        }
      }

      const alert_core::JsonValue previous_dom = store.rawDom();
      const RuntimeConfig previous_runtime = captureRuntime(store, previous_dom);

      internal::ConfigTransactionHooks hooks;
      hooks.commit_config = [&](std::string& error) { return store.commit(cmd.config, error); };
      hooks.apply_runtime = [&](std::string& error) {
        const RuntimeConfig desired = captureRuntime(store, cmd.config);
        return applyRuntime(desired, cmd.changed_sections, error);
      };
      hooks.rollback_config = [&](std::string& error) {
        return store.commit(previous_dom, error);
      };
      hooks.rollback_runtime = [&](std::string& error) {
        return applyRuntime(previous_runtime, cmd.changed_sections, error);
      };

      std::string error;
      res.ok = internal::runConfigTransaction(hooks, error);
      res.err = error;
      if (!res.ok) {
        ADMIN_LOG_ERROR("config transaction failed: %s", error.c_str());
        return;
      }
      ADMIN_LOG_INFO("config transaction applied");
      return;
    }

    case AdminCommandType::ApplyDashboard: {
      if (cmd.dashboard == nullptr) {
        res.err = "missing dashboard candidate";
        return;
      }
      bool conflict = false;
      res.ok = DashboardStore::instance().commit(
          *cmd.dashboard, cmd.dashboard_check_revision, cmd.dashboard_expected_revision,
          res.err, conflict);
      res.conflict = conflict;
      return;
    }

    case AdminCommandType::DeleteDashboard: {
      bool conflict = false;
      res.ok = DashboardStore::instance().erase(
          cmd.dashboard_check_revision, cmd.dashboard_expected_revision, res.err, conflict);
      res.conflict = conflict;
      return;
    }

    case AdminCommandType::WriteSourceCredential: {
      if (!net::CredentialStore::validSourceKeyForWrite(cmd.credential_key) ||
          !net::CredentialStore::validSourceValue(cmd.credential_value)) {
        res.err = "invalid source credential key or value";
        return;
      }
      const esp_err_t err = net::CredentialStore::saveSourceCredential(
          cmd.credential_key, cmd.credential_value);
      res.ok = err == ESP_OK;
      if (!res.ok) res.err = "failed to persist source credential";
      return;
    }

    case AdminCommandType::DeleteSourceCredential: {
      bool in_use = false;
      const esp_err_t err = internal::deleteSourceCredentialIfUnreferenced(
          cmd.credential_key, in_use);
      res.ok = err == ESP_OK;
      res.conflict = in_use;
      if (!res.ok) {
        res.err = in_use ? "credential is referenced by an active source configuration"
                         : "failed to delete source credential";
      }
      return;
    }

    case AdminCommandType::ApplySettings: {
      const settings::Snapshot snap = settings::DeviceSettings::instance().snapshot();
      settings::Settings candidate;
      settings::FieldErrors errors;
      if (!settings::mergePatch(snap.values, cmd.settings_patch, candidate, errors)) {
        res.ok = false;
        res.err = errors.empty() ? "settings validation failed"
                                 : errors.front().field + ": " + errors.front().message;
        return;
      }
      uint32_t changed = 0;
      std::string err;
      if (!settings::DeviceSettings::instance().update(candidate, &changed, err)) {
        res.ok = false;
        res.err = err;
        return;
      }
      ADMIN_LOG_INFO("device settings applied: mask=%u", static_cast<unsigned>(changed));
      res.ok = true;
      return;
    }

    case AdminCommandType::ApplyInputBindings: {
      if (cmd.input_bindings == nullptr) {
        res.err = "missing input binding candidate";
        return;
      }
      const auto document = DashboardStore::instance().snapshot();
      const dashboard::Dashboard* current =
          document != nullptr && document->configured ? &document->dashboard : nullptr;
      bool conflict = false;
      if (cmd.input_reset) {
        res.ok = InputBindings::instance().reset(true, cmd.input_bindings->revision, current,
                                                res.err, conflict);
      } else {
        res.ok = InputBindings::instance().update(
            cmd.input_bindings->bindings, true, cmd.input_bindings->revision, current,
            res.err, conflict);
      }
      res.conflict = conflict;
      return;
    }

    case AdminCommandType::ApplyWifi: {
      // NVS 为密码权威（M13 §4）；config wifi_list 仅 ssid+pass_ref 展示引用。
      std::vector<net::WifiAccount> previous;
      net::CredentialStore::loadWifiAccounts(previous);
      std::vector<net::WifiAccount> accounts;
      auto wipe = [&]() {
        for (auto& a : previous) net::CredentialStore::secureErase(a.password);
        for (auto& a : accounts) net::CredentialStore::secureErase(a.password);
      };
      if (cmd.wifi_list != nullptr) {
        net::WifiListError list_error;
        if (!net::applyWifiListEdit(previous, *cmd.wifi_list, accounts, list_error)) {
          wipe();
          res.ok = false;
          res.err = list_error.field + ": " + list_error.message;
          return;
        }
      } else {
        accounts = previous;
        if (cmd.wifi_remove) {
          accounts.erase(std::remove_if(accounts.begin(), accounts.end(),
                                        [&](const net::WifiAccount& a) {
                                          return a.ssid == cmd.wifi_ssid;
                                        }),
                         accounts.end());
        } else {
          bool replaced = false;
          for (auto& a : accounts) {
            if (a.ssid == cmd.wifi_ssid) {
              a.password = cmd.wifi_password;
              replaced = true;
              break;
            }
          }
          if (!replaced) {
            if (accounts.size() >= net::kMaxWifiNetworks) {
              wipe();
              res.ok = false;
              res.err = "wifi account list full (max " +
                        std::to_string(net::kMaxWifiNetworks) + ")";
              return;
            }
            accounts.push_back({cmd.wifi_ssid, cmd.wifi_password});
          }
        }
      }
      if (net::CredentialStore::saveWifiAccounts(accounts) != ESP_OK) {
        wipe();
        res.ok = false;
        res.err = "failed to persist wifi credentials to NVS";
        return;
      }
      alert_core::JsonValue dom = store.rawDom();
      std::vector<std::pair<std::string, std::string>> list;
      for (size_t i = 0; i < accounts.size(); i++) {
        list.emplace_back(accounts[i].ssid, "nvs:wifi/" + std::to_string(i));
      }
      std::string err;
      if (store.setWifiListInDom(dom, list) && store.commit(dom, err)) {
        ADMIN_LOG_INFO("wifi config changed: %s (accounts=%u)",
                       cmd.wifi_list ? "list replaced" : (cmd.wifi_remove ? "removed" : "saved"),
                       static_cast<unsigned>(accounts.size()));
      } else {
        ADMIN_LOG_ERROR("wifi_list config sync failed: %s", err.c_str());
      }
      // 只在影响当前连接时重连：未连接、当前网络被删除或其密码变化。
      // 纯排序/增删其他网络不打断现有连接（新顺序在下次断线重连时生效）。
      const auto& wifi = net::WifiManager::instance();
      bool reconnect = !wifi.isConnected();
      if (!reconnect) {
        const std::string current = wifi.currentSsid();
        const net::WifiAccount* before = nullptr;
        const net::WifiAccount* after = nullptr;
        for (const auto& a : previous) if (a.ssid == current) before = &a;
        for (const auto& a : accounts) if (a.ssid == current) after = &a;
        reconnect = after == nullptr || before == nullptr || before->password != after->password;
      }
      wipe();
      if (reconnect) net::NetStack::instance().reconnectNow(1000, wifi.isConnected());
      res.ok = true;
      return;
    }

    case AdminCommandType::WifiReset: {
      if (ble::BleManager::instance().provisioningActive()) {
        res.ok = false;
        res.err = "close BLE provisioning before WiFi reset";
        return;
      }
      ADMIN_LOG_WARN("wifi-only reset executing");
      if (net::CredentialStore::clearWifiAccounts() != ESP_OK) {
        res.ok = false;
        res.err = "failed to clear wifi credentials";
        return;
      }
      alert_core::JsonValue dom = store.rawDom();
      std::string err;
      if (!store.setWifiListInDom(dom, {}) || !store.commit(dom, err)) {
        ADMIN_LOG_ERROR("wifi reset: wifi_list sync failed: %s", err.c_str());
      }
      net::NetStack::instance().reconnectNow();
      const esp_err_t ap_err = ProvisionPortal::instance().openApFallback();
      if (ap_err != ESP_OK &&
          !(ap_err == ESP_ERR_INVALID_STATE && ProvisionPortal::instance().apActive())) {
        res.ok = false;
        res.err = "wifi credentials cleared but recovery AP failed";
        return;
      }
      res.ok = true;
      return;
    }

    case AdminCommandType::FactoryReset: {
      if (!::eego::SdCard::instance().ready()) {
        res.ok = false;
        res.err = "factory reset incomplete: SD unavailable; persistent state unchanged";
        return;
      }
      ADMIN_LOG_WARN("factory reset executing");
      bool ok = true;
      if (ble::BleManager::instance().clearBonds() != ESP_OK) {
        ADMIN_LOG_ERROR("factory reset: BLE stop/bond clear failed");
        ok = false;
      }
      net::NetStack::instance().stop();
      for (const char* ns : {"sec", "source", "wifi", "nimble_bond", "dashboard", "touchcal"}) {
        if (net::CredentialStore::clearNamespace(ns) != ESP_OK) {
          ADMIN_LOG_ERROR("factory reset: clear namespace failed: %s", ns);
          ok = false;
        }
      }
      if (settings::DeviceSettings::erasePersisted() != ESP_OK) {
        ADMIN_LOG_ERROR("factory reset: clear device settings failed");
        ok = false;
      }
      if (InputBindings::erasePersisted() != ESP_OK) {
        ADMIN_LOG_ERROR("factory reset: clear input bindings failed");
        ok = false;
      }
      AuthManager::instance().revokeAllSessions();
      if (!removeFactoryConfigs()) {
        ADMIN_LOG_ERROR("factory reset: config file removal incomplete");
        ok = false;
      }
      if (!ok) {
        res.ok = false;
        res.err = "factory reset incomplete; recovery required; reboot not scheduled";
        return;
      }
      res.ok = true;
      scheduleReboot(cmd.delay_ms > 0 ? cmd.delay_ms : 2000);
      return;
    }

    case AdminCommandType::BleClearBonds: {
      const esp_err_t err = ble::BleManager::instance().clearBonds();
      res.ok = err == ESP_OK;
      if (!res.ok) res.err = "BLE stop/bond clear failed";
      return;
    }

    case AdminCommandType::RenderPreview: {
      // app_task (UI owner): offscreen render via the registered provider.
      res.ok = cmd.preview != nullptr && internal::runRenderPreviewJob(*cmd.preview, res.err);
      if (cmd.preview == nullptr) res.err = "render preview job missing";
      return;
    }

    case AdminCommandType::Reboot: {
      res.ok = true;
      scheduleReboot(cmd.delay_ms > 0 ? cmd.delay_ms : 2000);
      return;
    }
  }
  res.ok = false;
  res.err = "unknown command";
}

}  // namespace eego::admin
