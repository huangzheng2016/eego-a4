// SPDX-License-Identifier: GPL-3.0-or-later
// admin 组件装配 —— 注入点存储、事件发射、默认命令处理器。
#include "admin/admin.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#include "admin_internal.h"
#include "device_settings/device_settings.h"
#include "esp_timer.h"
#include "net_stack/credentials.h"
#include "net_stack/net_task.h"
#include "sd_card.h"

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
      "/sdcard/alertpanel/alerts.db",
      "/sdcard/alertpanel/alerts.db.tmp",
      "/sdcard/alertpanel/dashboard.json",
      "/sdcard/alertpanel/dashboard.json.tmp",
      "/sdcard/alertpanel/dashboard.json.bak",
  };
  bool ok = true;
  for (const char* path : kPaths) {
    if (remove(path) != 0 && errno != ENOENT) ok = false;
  }
  return ok;
}

// sources 热生效（sources_changed 时调用；net_task 线程安全 API）。
// v1 语义：全量替换——移除所有现存源，按配置重新 addSource。
void syncSources(const std::vector<net::SourceConfig>& desired) {
  net::NetStack& ns = net::NetStack::instance();
  if (!ns.running()) {
    ADMIN_LOG_WARN("sources sync skipped: net_task not running");
    return;
  }
  net::NetStatusSnapshot snap;
  if (ns.queryStatus(snap)) {
    for (const auto& h : snap.sources) {
      ns.removeSource(h.id);
    }
  }
  for (const auto& cfg : desired) {
    std::string err;
    if (!ns.addSource(cfg, err)) {
      ADMIN_LOG_ERROR("source add failed: id=%s (%s)", cfg.id.c_str(), err.c_str());
    }
  }
  ADMIN_LOG_INFO("sources synced: %u source(s)", static_cast<unsigned>(desired.size()));
}

// mute_rules 热生效（M21；net_task 线程安全 API，全量替换语义与 store 一致）。
// net_task 未运行时跳过（规则已在 config.json，下次启动恢复，M21 组合根保证）。
void syncMuteRules(const std::vector<alert_core::MuteRule>& rules) {
  net::NetStack& ns = net::NetStack::instance();
  if (!ns.running()) {
    ADMIN_LOG_WARN("mute rules sync skipped: net_task not running");
    return;
  }
  std::string err;
  if (!ns.setMuteRules(rules, err)) {
    ADMIN_LOG_ERROR("mute rules sync failed: %s", err.c_str());
    return;
  }
  ADMIN_LOG_INFO("mute rules synced: %u rule(s)", static_cast<unsigned>(rules.size()));
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
      std::string err;
      if (!store.commit(cmd.config, err)) {
        res.ok = false;
        res.err = err;
        return;
      }
      if (cmd.changed_sections & AdminCommand::kChangedAdmin) {
        const AdminDomainConfig admin = store.admin();
        AuthManager::instance().setSessionTtl(admin.session_ttl_s);
        AuthManager::instance().setLockThreshold(admin.login_lock_threshold);
        LogLevel lv;
        if (logLevelFromString(admin.log_level, lv)) AdminLog::instance().setMinLevel(lv);
        ADMIN_LOG_INFO("config applied: admin");
      }
      // Legacy admin.display writes use the same DeviceSettings validator and
      // subscription path as typed settings. Other admin-only writes do not
      // reapply a possibly stale legacy display mirror.
      if (cmd.changed_sections & AdminCommand::kChangedAdminDisplay) {
        const alert_core::JsonValue* admin = cmd.config.find("admin");
        const alert_core::JsonValue* display = admin != nullptr ? admin->find("display") : nullptr;
        if (display != nullptr) {
          const settings::Snapshot snap = settings::DeviceSettings::instance().snapshot();
          settings::Settings candidate = snap.values;
          settings::applyLegacyDisplay(*display, candidate);
          if ((settings::changedFields(snap.values, candidate) & settings::kChangedFrontlight) != 0) {
            if (!snap.capabilities.frontlight) {
              ADMIN_LOG_WARN("legacy display ignored: frontlight unavailable");
            } else {
              uint32_t changed = 0;
              if (!settings::DeviceSettings::instance().update(candidate, &changed, err)) {
                res.ok = false;
                res.err = err;
                return;
              }
            }
          }
        }
      }
      if ((cmd.changed_sections & AdminCommand::kChangedOverrides) && g_registry != nullptr) {
        g_registry->clearOverrides();
        const alert_core::JsonValue* ov = cmd.config.find("alert_type_overrides");
        alert_core::ValidationError verr;
        if (ov != nullptr && !g_registry->applyOverridesJson(*ov, verr)) {
          ADMIN_LOG_ERROR("type overrides apply failed: %s.%s", verr.field.c_str(),
                          verr.message.c_str());
        }
        ADMIN_LOG_INFO("config applied: alert_type_overrides");
      }
      if (cmd.changed_sections & AdminCommand::kChangedSources) {
        syncSources(store.sources());
        ADMIN_LOG_INFO("config applied: sources");
      }
      if (cmd.changed_sections & AdminCommand::kChangedMuteRules) {
        syncMuteRules(store.muteRules());
        ADMIN_LOG_INFO("config applied: mute_rules");
      }
      res.ok = true;
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

    case AdminCommandType::ApplyWifi: {
      // NVS 为密码权威（M13 §4）；config wifi_list 仅 ssid+pass_ref 展示引用。
      std::vector<net::WifiAccount> accounts;
      net::CredentialStore::loadWifiAccounts(accounts);
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
          if (accounts.size() >= 4) {
            res.ok = false;
            res.err = "wifi account list full (max 4)";
            return;
          }
          accounts.push_back({cmd.wifi_ssid, cmd.wifi_password});
        }
      }
      if (net::CredentialStore::saveWifiAccounts(accounts) != ESP_OK) {
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
                       cmd.wifi_remove ? "removed" : "saved",
                       static_cast<unsigned>(accounts.size()));
      } else {
        ADMIN_LOG_ERROR("wifi_list config sync failed: %s", err.c_str());
      }
      net::NetStack::instance().reconnectNow();
      res.ok = true;
      return;
    }

    case AdminCommandType::WifiReset: {
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
      if (ap_err != ESP_OK && ap_err != ESP_ERR_INVALID_STATE) {
        res.ok = false;
        res.err = "wifi credentials cleared but recovery AP failed";
        return;
      }
      res.ok = true;
      return;
    }

    case AdminCommandType::FactoryReset: {
      ADMIN_LOG_WARN("factory reset executing");
      net::NetStack::instance().stop();
      bool ok = true;
      for (const char* ns : {"sec", "wifi", "ui", "ble"}) {
        if (net::CredentialStore::clearNamespace(ns) != ESP_OK) {
          ADMIN_LOG_ERROR("factory reset: clear namespace failed: %s", ns);
          ok = false;
        }
      }
      if (settings::DeviceSettings::erasePersisted() != ESP_OK) {
        ADMIN_LOG_ERROR("factory reset: clear device settings failed");
        ok = false;
      }
      AuthManager::instance().revokeAllSessions();
      if (::eego::SdCard::instance().ready() && !removeFactoryConfigs()) {
        ADMIN_LOG_ERROR("factory reset: config file removal incomplete");
        ok = false;
      }
      scheduleReboot(cmd.delay_ms > 0 ? cmd.delay_ms : 2000);
      if (!ok) {
        res.ok = false;
        res.err = "factory reset incomplete; rebooting for recovery";
        return;
      }
      res.ok = true;
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
