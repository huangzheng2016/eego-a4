// SPDX-License-Identifier: GPL-3.0-or-later
// admin 组件装配 —— 注入点存储、事件发射、默认命令处理器。
#include "admin/admin.h"

#include <algorithm>
#include <dirent.h>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

#include "admin_internal.h"
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

// 递归删除 SD 目录（恢复出厂：删配置目录保留字体，M13 F8）。
bool removeRecursive(const std::string& path) {
  DIR* dir = opendir(path.c_str());
  if (dir == nullptr) return true;  // 不存在视为成功
  bool ok = true;
  struct dirent* ent;
  while ((ent = readdir(dir)) != nullptr) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
    const std::string child = path + "/" + ent->d_name;
    if (ent->d_type == DT_DIR) {
      if (!removeRecursive(child)) ok = false;
    } else {
      if (remove(child.c_str()) != 0) ok = false;
    }
  }
  closedir(dir);
  if (rmdir(path.c_str()) != 0) ok = false;
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
      // 1. 原子持久化（tmp+rename+回读；失败 RAM 不变 → httpd 500）
      std::string err;
      if (!store.commit(cmd.config, err)) {
        res.ok = false;
        res.err = err;
        return;
      }
      // 2. admin 域热生效（日志只记 section 名，M13 F7）
      if (cmd.changed_sections & AdminCommand::kChangedAdmin) {
        const AdminDomainConfig admin = store.admin();
        AuthManager::instance().setSessionTtl(admin.session_ttl_s);
        AuthManager::instance().setLockThreshold(admin.login_lock_threshold);
        LogLevel lv;
        if (logLevelFromString(admin.log_level, lv)) AdminLog::instance().setMinLevel(lv);
        ADMIN_LOG_INFO("config applied: admin");
      }
      // 3. alert_type_overrides → 注册表（整段替换语义：先清再应用）
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
      // 4. sources 热生效（整段替换）
      if (cmd.changed_sections & AdminCommand::kChangedSources) {
        syncSources(store.sources());
        ADMIN_LOG_INFO("config applied: sources");
      }
      // 5. mute_rules 热生效（整段替换；M21）
      if (cmd.changed_sections & AdminCommand::kChangedMuteRules) {
        syncMuteRules(store.muteRules());
        ADMIN_LOG_INFO("config applied: mute_rules");
      }
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
      // config wifi_list 同步 + 原子提交（失败不阻断：NVS 已是权威）
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
      // 触发重连（net_task 上下文，线程安全）
      net::NetStack::instance().reconnectNow();
      res.ok = true;
      return;
    }

    case AdminCommandType::FactoryReset: {
      // M13 F8：清 NVS sec/wifi + 删 SD 配置目录（保留字体）→ 重启
      ADMIN_LOG_WARN("factory reset executing");
      net::CredentialStore::clearNamespace("sec");
      net::CredentialStore::clearNamespace("wifi");
      AuthManager::instance().revokeAllSessions();
      if (::eego::SdCard::instance().ready()) {
        if (!removeRecursive("/sdcard/alertpanel")) {
          ADMIN_LOG_ERROR("factory reset: config dir removal incomplete");
        }
      }
      res.ok = true;  // 先回 httpd 202，再延迟重启
      scheduleReboot(cmd.delay_ms > 0 ? cmd.delay_ms : 2000);
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
