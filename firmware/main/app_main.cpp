// SPDX-License-Identifier: GPL-3.0-or-later
// EEGO A4 告警/数据面板 —— FW5 组合根（M18）+ FW5B（M21）+ DP4 dashboard（M26）。
//
// 启动流程（顺序即接线合同，M12 架构）：
//   1. board::beginStandardHardware() 板级启动合同（GPIO4 锁存 → 触控/按键 →
//      前光探测 → 充电极性 → RTC → SD → 显示）
//   2. 统一时间源启动同步：RTC 有效 → settimeofday 设置系统时间（M21；
//      SNTP 前的 TLS/日志/摄入都依赖系统时间）
//   3. NVS（CredentialStore::ensureNvs）+ AuthManager + ConfigStore(config.json)
//      + 类型覆盖应用到注册表 + AdminLog（级别取 admin.log_level）
//   4. 字体：admin.font 指定路径 > SD 卡首个 .eefont/.bin > 内置 5x7 回退
//   5. 告警库：SdAlertFileIo + AlertDb(/sdcard/alertpanel/alerts.db) →
//      AlertStore.loadFrom 恢复 → attachDb（此后 store 写只在 net_task）
//   6. NetStack::start（注入 SnapshotPublisher/统一时间源/RTC 回写；expire
//      巡检与首帧快照发布由 net_task 承担）→ 按 config.sources 逐个 addSource
//      → 恢复静音规则（config.mute_rules；FW5 NVS 指纹表一次性迁移并入）
//   7. admin 注入：快照源/动作 sink（薄适配）/命令处理器/事件回调
//   8. 模式判定：无管理密码 → 配网门户 + UI 配网页；否则 startAdminApi
//   9. diag::start（串口命令钩子全部在 app_task 上下文执行）+ PanelApp.begin
//
// app_task 主循环（唯一 UI 属主）：
//   触控 8ms 轮询 → 按键队列 → diag::service → admin::serviceCommands(0) →
//   app.service()（事件归并/电源 FSM/按需渲染）→ vTaskDelay(8ms)
//
// 串口诊断入口：UART0 115200，输入 help 查看命令。

#include "board/board_boot.h"
#include "eego_hal.h"
#include "eego_ui.h"

#include <cstdio>
#include <cstring>
#include <strings.h>  // strcasecmp

#include "admin/admin.h"
#include "alert_core/persistence.h"
#include "alert_core/store.h"
#include "alert_core/type_registry.h"
#include "diag/diag.h"
#include "device_prefs.h"
#include "device_settings/device_settings.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_stack/net_stack.h"
#include "net_stack/sd_alert_file_io.h"
#include "panel_app.h"
#include "time_source.h"

namespace admin = eego::admin;
namespace diag = eego::diag;

namespace {

const char* TAG = "app";

constexpr uint32_t kTouchPollMs = 8;  // GSLX680 轮询周期（触控合同）

// ── 组合根持有的长生命周期对象（store/registry/publisher 必须 outlive NetStack）──

eego::Uc8279cDisplay display;
eego::Gslx680Touch touch;
eego::PortraitCanvas canvas(display);
eego::board::BootStatus bootStatus;

alert_core::SnapshotPublisher publisher;
alert_core::TypeRegistry registry;

// ULID 熵源：ESP32-S3 硬件 RNG（M13：id 不可预测）
void entropyHook(uint8_t* out, size_t n) {
  size_t i = 0;
  while (i < n) {
    const uint32_t r = esp_random();
    const size_t chunk = (n - i < sizeof(r)) ? (n - i) : sizeof(r);
    memcpy(out + i, &r, chunk);
    i += chunk;
  }
}

alert_core::AlertStore store(registry, entropyHook);

// 持久化：SD/FatFS 版 IAlertFileIo + JSONL alerts.db（先 load 后 start）
eego::net::SdAlertFileIo alertFileIo;
alert_core::AlertDb alertDb(alertFileIo, "/sdcard/alertpanel/alerts.db");

panel::PanelApp app;

// ── 字体 ─────────────────────────────────────────────────────────────────────

// admin.font 指定的 SD 绝对路径优先；否则扫 SD 根目录首个 .eefont/.bin；
// 都失败保持内置 5x7 回退（面板始终可显示文字）。
void tryLoadUiFont() {
  const std::string configured = admin::ConfigStore::instance().admin().font;
  if (!configured.empty()) {
    if (eego::FontManager::instance().loadUiFont(configured.c_str())) {
      ESP_LOGI(TAG, "UI font loaded (config): %s", configured.c_str());
      return;
    }
    ESP_LOGW(TAG, "configured font failed, fallback to scan: %s", configured.c_str());
  }
  if (!eego::SdCard::instance().ready()) return;
  eego::SdCard::DirEntry entries[16];
  const int n = eego::SdCard::instance().listDir("/", entries, 16);
  for (int i = 0; i < n; i++) {
    if (entries[i].isDir) continue;
    const char* dot = strrchr(entries[i].name, '.');
    if (!dot) continue;
    if (strcasecmp(dot, ".eefont") != 0 && strcasecmp(dot, ".bin") != 0) continue;
    char path[258];  // '/' + name(255) + NUL
    snprintf(path, sizeof(path), "/%s", entries[i].name);
    if (eego::FontManager::instance().loadUiFont(path)) {
      ESP_LOGI(TAG, "UI font loaded: %s", path);
      return;
    }
  }
  ESP_LOGI(TAG, "no SD font found, using built-in 5x7 fallback");
}

// ── 跨任务事件回调（短平快：只置原子标志/投小信箱，不做渲染/IO）──────────────

void onNetEvent(const eego::net::NetEvent& ev, void* ctx) {
  static_cast<panel::PanelApp*>(ctx)->notifyNetEvent(
      ev.wifi_state, ev.type == eego::net::NetEventType::SourceHealthChanged);
}

void onAdminEvent(const eego::admin::AdminEvent& ev, void* ctx) {
  int reason = -1;
  if (ev.reason != nullptr) {
    if (strcmp(ev.reason, "expired") == 0) reason = 0;
    else if (strcmp(ev.reason, "success") == 0) reason = 1;
    else if (strcmp(ev.reason, "manual") == 0) reason = 2;
  }
  static_cast<panel::PanelApp*>(ctx)->notifyAdminEvent(static_cast<int>(ev.type), reason,
                                                       ev.window_remaining_s);
}

// net::AlertActionResult → admin::AlertActionResult 薄适配（M17/M20 同构注入点）。
// 显式逐码映射（不依赖枚举数值一致，防未来重排）。
admin::AlertActionResult adaptActionResult(const eego::net::AlertActionResult& r) {
  admin::AlertActionResult out;
  switch (r.code) {
    case eego::net::AlertActionResult::Code::Ok:
      out.code = admin::AlertActionResultCode::Ok;
      break;
    case eego::net::AlertActionResult::Code::NotFound:
      out.code = admin::AlertActionResultCode::NotFound;
      break;
    case eego::net::AlertActionResult::Code::InvalidState:
      out.code = admin::AlertActionResultCode::InvalidState;
      break;
    case eego::net::AlertActionResult::Code::InvalidAction:
      out.code = admin::AlertActionResultCode::InvalidAction;
      break;
    case eego::net::AlertActionResult::Code::NoStore:
      out.code = admin::AlertActionResultCode::NoStore;
      break;
    default:
      out.code = admin::AlertActionResultCode::Unavailable;
      break;
  }
  out.affected = r.affected;
  out.detail = r.detail;
  return out;
}

// 统一时间源已收敛到 time_source.h（M21）：TimeSource::now/valid/writeRtc，
// RTC 句柄为函数内静态（无状态驱动对象，I2C 端口级锁跨任务安全）。

}  // namespace

extern "C" void app_main(void) {
  ESP_LOGI(TAG, "EEGO A4 alert panel boot (FW5 panel app + diagnostics)");

  // 1. 板级启动合同（顺序即合同，见 board_boot.h）
  bootStatus = eego::board::beginStandardHardware(display, touch);

  // 2. 统一时间源启动同步（M21）：RTC 有效而系统时间不可信 → 设置系统时间
  //    （SNTP 前的 TLS 证书校验/日志/摄入都依赖系统时间）
  if (panel::TimeSource::syncSystemFromRtc()) {
    ESP_LOGI(TAG, "system time set from RTC");
  }

  // 3. NVS / 认证 / 配置存储 / 类型覆盖 / 管理日志
  if (eego::net::CredentialStore::ensureNvs() != ESP_OK) {
    ESP_LOGE(TAG, "NVS init failed; credentials/config persistence unavailable");
  }
  admin::AuthManager::instance().init();
  admin::ConfigStore& config = admin::ConfigStore::instance();
  config.init("/sdcard/alertpanel/config.json");
  if (!config.valid()) {
    ESP_LOGW(TAG, "config.json invalid (backup made), running on defaults");
  }
  const alert_core::JsonValue dom = config.rawDom();
  const alert_core::JsonValue* overrides = dom.find("alert_type_overrides");
  if (overrides != nullptr) {
    alert_core::ValidationError verr;
    if (!registry.applyOverridesJson(*overrides, verr)) {
      ESP_LOGW(TAG, "type overrides apply failed: %s.%s", verr.field.c_str(),
               verr.message.c_str());
    }
  }
  eego::admin::LogLevel logLevel;
  if (eego::admin::logLevelFromString(config.admin().log_level, logLevel)) {
    eego::admin::AdminLog::instance().init(logLevel);
  } else {
    eego::admin::AdminLog::instance().init();
  }

  const alert_core::JsonValue* adminSection = dom.find("admin");
  const alert_core::JsonValue* legacyDisplay =
      adminSection != nullptr ? adminSection->find("display") : nullptr;
  auto& deviceSettings = eego::settings::DeviceSettings::instance();
  const esp_err_t settingsErr = deviceSettings.init(legacyDisplay);
  if (settingsErr != ESP_OK) {
    ESP_LOGE(TAG, "device settings init failed: %s", esp_err_to_name(settingsErr));
  }
  eego::settings::Capabilities capabilities;
  capabilities.frontlight = eego::FrontlightManager::instance().present();
  capabilities.battery = eego::PowerManager::instance().readBattery().millivolts > 0;
  capabilities.bluetooth = false;
  capabilities.display = bootStatus.displayReady;
  capabilities.sd = bootStatus.sdReady;
  capabilities.rtc = bootStatus.rtcReady;
  deviceSettings.setCapabilities(capabilities);
  const esp_err_t inputErr = admin::InputBindings::instance().init();
  if (inputErr != ESP_OK) {
    ESP_LOGE(TAG, "input bindings init failed: %s; using safe defaults",
             esp_err_to_name(inputErr));
  }

  // 4. 字体
  tryLoadUiFont();

  // 5. 告警库恢复（store 写自此只在 net_task；load 先于 start，首帧快照覆盖）
  if (eego::SdCard::instance().ready()) {
    const size_t loaded = store.loadFrom(alertDb);
    ESP_LOGI(TAG, "alerts.db restored: %u record(s)", static_cast<unsigned>(loaded));
  } else {
    ESP_LOGW(TAG, "SD not ready: alerts.db unavailable, running in-memory");
  }
  store.attachDb(&alertDb);

  // 6. net_task 启动（快照发布 + expire 巡检由 net_task 承担；统一时间源 +
  //    RTC 回写注入，M21）
  eego::net::NetStackConfig netCfg;
  netCfg.snapshot_publisher = &publisher;
  netCfg.rtc_write_fn = &panel::TimeSource::writeRtc;
  netCfg.time_valid_fn = &panel::TimeSource::valid;
  const esp_err_t netErr =
      eego::net::NetStack::instance().start(netCfg, &store, &registry, &panel::TimeSource::now);
  if (netErr != ESP_OK) {
    ESP_LOGE(TAG, "net_task start failed: %s", esp_err_to_name(netErr));
  }
  for (const eego::net::SourceConfig& sc : config.sources()) {
    std::string err;
    if (!eego::net::NetStack::instance().addSource(sc, err)) {
      ESP_LOGW(TAG, "source add failed: id=%s (%s)", sc.id.c_str(), err.c_str());
    }
  }

  // 6b. 静音规则恢复（M21）：config.mute_rules → net_task（重估已恢复记录的
  //     muted 标志 + 快照发布）。FW5 的 NVS UI 指纹表在此一次性迁移为 store
  //     级指纹规则：合并表先落盘 config.json（与面板 persistMuteRules 同路径），
  //     成功后才清 NVS 键——commit 失败时 NVS 是唯一副本，保留待下次启动重试。
  {
    std::vector<alert_core::MuteRule> mute_rules = config.muteRules();
    panel::DevicePrefs& legacy_prefs = panel::DevicePrefs::instance();
    legacy_prefs.load();  // 只读旧 mute 键；其他 ui 偏好已由 DeviceSettings 迁移
    size_t migrated = 0;
    for (const std::string& fp : legacy_prefs.mutes()) {
      bool dup = false;
      for (const auto& r : mute_rules) {
        if (r.match_by == alert_core::MuteRule::MatchBy::Fingerprint && r.value == fp) {
          dup = true;
          break;
        }
      }
      if (!dup) {
        alert_core::MuteRule rule;
        rule.match_by = alert_core::MuteRule::MatchBy::Fingerprint;
        rule.value = fp;
        rule.expires_at = 0;  // 面板静音语义：不过期
        mute_rules.push_back(std::move(rule));
        migrated++;
      }
    }
    bool persist_ok = true;
    if (migrated > 0) {
      alert_core::JsonValue dom = config.rawDom();
      std::string cerr;
      persist_ok = config.setMuteRulesInDom(dom, mute_rules) && config.commit(dom, cerr);
      if (persist_ok) {
        ESP_LOGI(TAG, "legacy UI mutes migrated to store rules: %u (persisted to config.json)",
                 static_cast<unsigned>(migrated));
      } else {
        ESP_LOGE(TAG, "legacy mute migration persist failed, NVS copy kept (%s)",
                 cerr.c_str());
      }
    }
    if (!legacy_prefs.mutes().empty() && persist_ok) {
      legacy_prefs.clearMutes();
      legacy_prefs.save();
    }
    std::string mute_err;
    if (!eego::net::NetStack::instance().setMuteRules(mute_rules, mute_err)) {
      ESP_LOGW(TAG, "mute rules restore failed: %s", mute_err.c_str());
    }
  }

  // 7. admin 注入（M17 同构注入点 + M20 现成 functor/薄适配）
  eego::net::NetStack& net = eego::net::NetStack::instance();
  admin::setAlertSnapshotSource(net.alertSnapshotSource());
  admin::setAlertActionSink(
      [](admin::AlertActionKind kind, const std::string& id, const std::string& ack_by) {
        eego::net::AlertAction action;
        switch (kind) {
          case admin::AlertActionKind::Ack:
            action.type = eego::net::AlertActionType::Ack;
            break;
          case admin::AlertActionKind::AckAll:
            action.type = eego::net::AlertActionType::AckAll;
            break;
          default:
            action.type = eego::net::AlertActionType::Clear;
            break;
        }
        action.id = id;
        action.ack_by = ack_by;
        return adaptActionResult(eego::net::NetStack::instance().alertAction(action));
      });
  admin::setAdminCommandHandler(&admin::defaultCommandHandler, nullptr);
  admin::setAdminEventCallback(&onAdminEvent, &app);
  net.setEventCallback(&onNetEvent, &app);

  // 8. 设备模式判定（M13 bootstrap）：无管理密码 → 配网门户；否则管理 API
  if (!admin::AuthManager::instance().hasPassword()) {
    ESP_LOGI(TAG, "no admin password: opening provisioning portal");
    if (admin::ProvisionPortal::instance().open() == ESP_OK) {
      app.enterProvision();
    }
  } else {
    admin::AdminConfig acfg;
    acfg.registry = &registry;
    const esp_err_t admErr = admin::startAdminApi(acfg);
    if (admErr != ESP_OK) {
      ESP_LOGW(TAG, "admin API start failed: %s", esp_err_to_name(admErr));
    }
  }

  // 9. 串口诊断（钩子全部在 app_task 上下文执行）+ UI 属主启动
  diag::Handlers hooks;
  hooks.status = [] { app.diagStatus(); };
  hooks.battery = [] { app.diagBattery(); };
  hooks.rtc = [] { app.diagRtc(); };
  hooks.sd = [] { app.diagSd(); };
  hooks.net = [] { app.diagNet(); };
  hooks.alerts = [] { app.diagAlerts(); };
  hooks.inject = [](const char* json) { app.diagInject(json); };
  hooks.ack_all = [] { app.diagAckAll(); };
  hooks.smoke = [] { app.diagSmoke(); };
  hooks.version = [] { app.diagVersion(); };
  hooks.lang = [](const char* arg) { app.diagLang(arg); };
  hooks.page = [](const char* arg) { app.diagPage(arg); };
  hooks.dashboard = [](const char* arg) { app.diagDashboard(arg); };
  if (!diag::start(hooks)) {
    ESP_LOGW(TAG, "diag start failed (serial diagnostics unavailable)");
  }

  panel::PanelApp::Deps deps;
  deps.display = &display;
  deps.touch = &touch;
  deps.canvas = &canvas;
  deps.boot = bootStatus;
  deps.publisher = &publisher;
  deps.registry = &registry;
  app.begin(deps);

  // 10. app_task 主循环（唯一 UI 属主；input/net/httpd/serial 只经队列/快照交互）
  ESP_LOGI(TAG, "app_task running");
  eego::ButtonEvent bevt;
  while (true) {
    // 触控轮询（8ms 合同，边沿触发；前键短/长按经触控 sentinel 上报）
    const eego::TouchEvent tev = touch.poll();
    if (tev.type != eego::TouchEventType::None) {
      app.handleTouch(tev);
    }

    // 按键事件队列（后台任务去抖）
    while (eego::ButtonManager::instance().poll(&bevt, 0)) {
      app.handleButton(bevt);
    }

    // 诊断开关热生效；禁用时不分发串口命令。
    if (deviceSettings.snapshot().values.diagnostics_enabled) diag::service();

    // admin 命令队列（httpd → app_task；0ms = 非阻塞轮询）
    admin::serviceCommands(0);

    // 应用主服务（事件归并/电源 FSM/按需渲染；电池模式可能在此深睡不返回）
    app.service();

    vTaskDelay(pdMS_TO_TICKS(kTouchPollMs));
  }
}
