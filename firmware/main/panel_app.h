// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// panel_app —— 监控面板设备应用状态机（组合根的 UI 属主）。
//
// 屏幕（产品模型：web 端配置的 dashboard 是唯一主界面）：
//   Dashboard     主页（已配置 dashboard）；告警通过告警面板/横幅呈现
//   Onboarding    主页（未配置）：设备地址、WiFi 状态与配置步骤
//   AlertDetail   告警详情：从告警面板/横幅进入，↑/↓ 浏览同一列表，确认/静音/删除
//   Status        设备状态：网络、电源、存储、固件、各数据源最近更新与错误
//   Settings / Help / Provision
//
// 导航（详见 README「设备操作」）：↑/↓ 短按 = 上/下一屏（列表内上/下一项）；
// ↑/↓ 长按 = 面板焦点；Power 短按 = 选择/确认，长按 = 返回；前置键短按 = 主页，
// 长按 = 设置，双击 = 状态（后两者及 ↑/↓ 可经 input-bindings 自定义）。
//
// 并发模型（M12 定稿，本类强制执行）：
//   * 全部渲染/导航/本地动作只在 app_task 上下文（begin/service/handle* 由
//     app_main 主循环调用）；触控 8ms 轮询与按键队列在 app_main 泵入；
//   * 告警数据只读：SnapshotPublisher::acquire 无锁取不可变快照，绝不直写
//     AlertStore；告警动作（ack/clear）与静音规则（set/clear/query）一律经
//     NetStack 命令队列由 net_task 执行；本类不触碰 HttpPool；
//   * 跨任务入口只有 notifyNetEvent / notifyAdminEvent：只做原子置位与小
//     信箱投递，短平快，绝不做渲染/IO。
//
// 电源双策略（M13/M18；电池模式详见 README「电池模式」与 battery_policy.h）：
//   * 常供电：WiFi modem-sleep（setPowerSave(true)）常驻，http_push 即时可达；
//   * 电池·定时唤醒（Fetching）：最小初始化 → WiFi 快连 → 一批拉取全部到期源
//     （硬预算）→ 离屏渲染 + 帧哈希 → 不变则不碰屏，否则 Fast（预载上一帧）/Full
//     → 深睡到下一个本地墙钟对齐边界；
//   * 电池·按键唤醒（Interactive）：完整应用，battery_interactive_timeout_s 无操作
//     后同样渲染深睡帧并对齐深睡；
//   * 夜间暂停（两种模式）：窗口内不唤醒，进入前状态行一次性显示恢复时刻。
//   模式判定：DeviceSettings Auto = 充电状态（无电池读数回退常供电），S4 可强制。
//
// 静音（store 级）：外部告警详情静音 = 指纹规则经 net_task 落入 AlertStore
// （重估 muted 标志 + 快照发布），并持久化到 config.json mute_rules（重启
// 恢复）；设置页清除全部静音 = 清空规则表。muted 记录不进入 dashboard 事件。

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "alert_core/snapshot.h"
#include "battery_mode.h"
#include "alert_core/type_registry.h"
#include "board/board_boot.h"
#include "dashboard_runtime.h"
#include "device_settings/device_settings.h"
#include "eego_hal.h"
#include "eego_ui.h"
#include "net_stack/net_status.h"
#include "panel_render.h"
#include "panel_types.h"
#include "refresh_policy.h"
#include "ui_text.h"
#include "admin/input_binding_model.h"
#include "admin/render_preview.h"
#include "input_confirm.h"
#include "input_navigation.h"

// 仅声明引用所需（定义见 alert_core/store.h，panel_app.cpp 经 net_stack 获得）
namespace alert_core {
struct MuteRule;
}  // namespace alert_core

namespace panel {

// 危险确认契约：布防后必须由同一控件重新持续按住；网络恢复、WiFi 重置、
// 恢复出厂、BLE 配网与解绑只接受实体 Power fresh hold。其他输入、目标/快照变化取消。

class PanelApp {
 public:
  struct Deps {
    eego::Uc8279cDisplay* display = nullptr;
    eego::Gslx680Touch* touch = nullptr;
    eego::PortraitCanvas* canvas = nullptr;
    eego::board::BootStatus boot;
    alert_core::SnapshotPublisher* publisher = nullptr;  // 必须 outlive PanelApp
    alert_core::TypeRegistry* registry = nullptr;        // 必须 outlive PanelApp
    // 电池定时唤醒快速路径（app_main 已跳过触控/BLE/管理 API，显示控制器延迟上电）。
    bool fastWake = false;
  };

  PanelApp() = default;
  PanelApp(const PanelApp&) = delete;
  PanelApp& operator=(const PanelApp&) = delete;

  // 初始化状态（读 DeviceSettings、应用语言/前光/电源模式、首帧全量渲染）。
  void begin(const Deps& deps);

  // app_task 主循环每轮调用：事件归并 → 电源 FSM → 按需渲染。
  void service();

  // 输入事件（app_task 上下文；app_main 泵自 touch.poll / ButtonManager）。
  void handleTouch(const eego::TouchEvent& ev);
  void handleButton(const eego::ButtonEvent& ev);
  // 记录 app_task 从阻塞操作（渲染/命令处理/空闲等待）恢复的时刻。输入陈旧度
  // 按 now - max(事件时间, 恢复时刻) 计：阻塞期间由触控/按键任务排队的手势
  // 不因 app_task 忙而被当作陈旧丢弃。
  void noteInputResume();

  // 管理 API 渲染预览（app_task 上下文，经 AdminCommand::RenderPreview 调用）：
  // 在独立的离屏 framebuffer 上渲染候选 dashboard，绝不触碰实屏帧缓冲。
  bool renderDashboardPreview(const eego::admin::RenderPreviewRequest& request, uint8_t* rows,
                              size_t stride, eego::admin::RenderPreviewResult& result);

  // 跨任务通知（必须短平快；可在 net_task/httpd/sys_evt 上下文调用）。
  void notifyNetEvent(int wifiState);
  void notifyAdminEvent(int type, int reason, uint32_t remainingS);

  // ---- diag 串口命令钩子（app_task 上下文；app_main 注册到 diag::Handlers）----
  void diagStatus();
  void diagBattery();
  void diagRtc();
  void diagSd();
  void diagNet();
  void diagAlerts();
  void diagInject(const char* json);
  void diagAckAll();
  void diagSmoke();
  void diagVersion();
  void diagLang(const char* arg);
  void diagPage(const char* arg);
  void diagDashboard(const char* arg);

  // 组合根在 admin 模式判定后调用：进入/退出配网页。
  void enterProvision();
  void leaveProvision();

 private:
  // ---- 事件处理（按页面分发）----
  void onTouchTap(int16_t x, int16_t y);
  void dispatchTrigger(eego::admin::InputTrigger trigger, int64_t now_us);
  void executeBinding(const eego::admin::InputBinding& binding, int64_t now_us);
  void executeBuiltin(const std::string& name, int64_t now_us);

  // ---- 导航 ----
  Page home() const { return homePage(dashboard_.available()); }
  void gotoPage(Page p);          // 切页（下一帧强制 Full）
  void goHome();                  // 前置键短按：主页（dashboard 默认页第一屏）
  void back();                    // 电源键长按/返回软键
  // Focus = 面板焦点（dashboard）/ 设置子页 / 软键焦点；
  // Page  = 上/下一屏（dashboard）/ 列表上/下一项（设置行、告警）/ 状态子页。
  void moveFocus(int delta);
  void movePage(int delta);
  void requestNavigation(NavigationKind kind, int delta, int64_t now_us);
  void flushNavigation(int64_t now_us);
  bool inputBusy() const;
  uint64_t inputEpoch() const;
  std::string pageContext() const;
  void activateFocused();         // 电源键短按
  void activateSoftKey(size_t idx);

  // ---- S4 设置（数据驱动：focus_ = visibleSettings() 下标，按 geom::kSettingsRowsPerPage 分页）----
  std::vector<SettingId> visibleSettings() const;
  static std::vector<SettingId> visibleSettingsFor(bool frontlight);
  // 上次排版时前光三行是否可见：硬件覆盖变化时据此把 focus_ 按 SettingId 重映射。
  bool frontlightRowsVisible_ = false;
  void activateSettingRow(SettingId id);               // 动作行执行 / 取值行前进一档（循环）
  // 取值行 ±1 档：触控左右半默认两端钳制；wrap=true（Power/点标签循环前进）到端回绕。
  bool adjustSettingRow(SettingId id, int dir, bool coarse = false, bool wrap = false);
  static bool settingAdjustable(SettingId id);
  size_t settingsPage() const;
  bool pageEdge(int delta) const;  // Settings/Status 当前子页沿 delta 方向已到边界
  size_t statusPageCount() const;

  // ---- 触控命中 ----
  struct TouchHit {
    enum class Kind : uint8_t { None, SoftKey, Row, PagePrev, PageNext };
    Kind kind = Kind::None;
    int index = -1;          // SoftKey: 软键下标；Row: 全局行下标（S4 = visibleSettings 下标）
    bool valueLeft = false;  // Row 且落在值列左半
    bool valueRight = false; // Row 且落在值列右半
  };
  TouchHit resolveTouchHit(int16_t x, int16_t y) const;
  std::string controlForHit(const TouchHit& hit) const;
  std::string focusedControl() const;
  std::string touchControl(int16_t x, int16_t y) const;
  void onTouchSwipe(eego::TouchEventType type, int64_t now_us);
  void onTouchHold(int16_t x, int16_t y);

  // ---- 双击门控：仅当前页存在 up.double/down.double 有效绑定时开启 ----
  void syncDoublePress(bool force);

  // ---- 动作（NetStack / DeviceSettings / recovery）----
  void doAck();                   // 告警详情：确认当前告警
  void doClear(const std::string& id);
  void doMute(const alert_core::AlertRecord& rec);
  void doClearMutes();
  void persistMuteRules(const std::vector<alert_core::MuteRule>& rules);
  void refreshMuteRuleCount();
  void doPollNow();
  void doOpenRecovery();
  void doStartBleProvision();
  void doClearBleBonds();
  void clearBlePop();
  void doWifiReset();
  void doFactoryReset();
  void resetInputBindings();
  bool updateSettings(const eego::settings::Settings& candidate);
  void applySettings(uint32_t changed);
  static void onSettingsChanged(const eego::settings::Settings& values, uint32_t changed,
                                void* ctx);
  // 电池模式下次唤醒（设置页「下次唤醒」）；常供电/无计划返回 0。
  uint64_t plannedWakeAt();
  battery::WakeInputs wakeInputs(uint64_t nowUnix) const;
  uint64_t currentSnapshotEpoch() const;
  void armConfirm(ConfirmKind kind);
  void armDashboardConfirm(const DashboardConfirmRequest& request);
  bool confirmTargetCurrent() const;
  void executeArmedConfirm();
  void clearConfirm();
  void showBanner(const char* text, uint32_t ms);
  void showBannerFmt(uint32_t ms, const char* fmt, ...)
      __attribute__((format(printf, 3, 4)));

  // ---- 电源模式 / 电池阶段机 ----
  bool computeMains();
  void onModeChange(bool mains);
  bool dashboardLive() const { return mains_ || battPhase_ == BattPhase::Interactive; }
  void enterInteractive(int64_t nowMs);
  void batteryService(int64_t nowMs);
  // 非阻塞：取消在途取数并登记休眠；serviceSleep() 等网络静默（有预算）后 sleepNow()。
  void requestSleep(int64_t nowMs, const char* why);
  void serviceSleep(int64_t nowMs);
  // 计算唤醒计划 → 渲染/判定/刷新深睡帧 → 持久化 → 深睡（不返回）。
  [[noreturn]] void sleepNow();
  // 深睡帧：离屏渲染、帧哈希、跳过/Fast/Full、保存到 SD 与 RTC。
  void presentSleepFrame(const battery::WakePlan& plan, uint64_t nowUnix);
  std::string batteryStatusText(uint8_t bucket, uint64_t stamp, uint32_t intervalMin) const;
  std::string nightPauseText(uint64_t resumeUnix) const;
  void drawNotice(const char* title, const char* line1, const char* line2, const char* line3);
  uint64_t frameHash() const;
  bool batchSucceeded() const;

  // 非阻塞网络静默等待：按退避间隔发短超时 waitIdle 探测（单次 ≤ ~200ms），
  // 预算耗尽返回 -1；静默返回 1；尚未确定返回 0。
  struct NetIdleWait {
    bool active = false;
    int64_t deadlineMs = 0;
    int64_t nextProbeMs = 0;
    int64_t intervalMs = 0;
  };
  static void startNetIdleWait(NetIdleWait& w, int64_t nowMs, int64_t budgetMs);
  static int pollNetIdle(NetIdleWait& w, int64_t nowMs);

  // ---- 空闲感知刷新节奏（refresh_policy.h）----
  // 用户交互（触控/按键/前置键/web 应用/设置变更）：切回 Active，待刷新内容立即显示。
  void noteInteraction(int64_t nowMs);
  void configureRefreshPolicy();
  // 有待显示内容时调用：Active → true；Idle → 仅墙钟边界或新严重告警时 true。
  bool displayAllowed(int64_t nowMs);
  void noteDisplayed();

  // ---- 渲染 ----
  void markDirty(bool forceFull = false) {
    dirty_ = true;
    if (forceFull) forceFull_ = true;
  }
  void render(int64_t nowMs);
  void buildStatusBar(StatusBarModel& m);
  void buildOnboarding(OnboardingModel& m);
  void buildDetail(AlertDetailModel& m);
  void buildStatus(KvModel& m);
  void buildS4(S4Model& m);
  void buildHelp(KvModel& m);

  // ---- 告警详情（dashboard 告警视图；外部告警附带 alert_core 记录）----
  void openDetail(const std::string& panel_id, const std::string& identity);
  void refreshDetailItems();
  const DashboardAlertItem* detailItem() const;
  const alert_core::AlertRecord* detailRecord() const;  // 外部告警的 store 记录
  void moveDetail(int delta);
  std::string detailSignature() const;

  // ---- 数据辅助 ----
  uint64_t nowUnix();             // 统一时间源（TimeSource：SNTP 系统时间 > RTC > 0）
  void refreshBattery(int64_t nowMs);
  const eego::net::NetStatusSnapshot& netStatus();  // 5s 缓存的源/WiFi/时间状态
  const alert_core::AlertRecord* findRecord(const std::string& id) const;
  void formatAge(uint64_t ts, char* out, size_t cap);
  void formatTs(uint64_t ts, char* out, size_t cap) const;
  const char* typeName(const std::string& typeId, std::string& buf) const;
  size_t softKeyCount() const;
  void softKeys(SoftKeyModel* out, size_t cap) const;
  int64_t nowMs() const;

  // ---- 跨任务信箱 ----
  void drainAdminMailbox();
  void drainNetBits();

  Deps deps_;
  DashboardRuntime dashboard_;
  eego::settings::Settings settings_;
  int settingsSubscription_ = -1;

  // 页面/导航状态（仅 app_task 上下文读写）
  Page page_ = Page::Onboarding;
  Page helpReturn_ = Page::Settings;
  size_t focus_ = 0;
  size_t statusPage_ = 0;
  size_t statusRows_ = 0;  // 上次构建的状态页总行数（分页用）
  // 告警详情：来源面板（空 = 全部告警）、当前条目身份与下标（条目消失时按下标就近）。
  std::string detailPanel_;
  std::string detailIdentity_;
  size_t detailIndex_ = 0;
  std::vector<DashboardAlertItem> detailItems_;
  std::string detailRendered_;
  int64_t lastDetailCheckMs_ = 0;
  bool provisionSucceeded_ = false;
  uint64_t pageEpoch_ = 1;
  NavigationCoalescer navigation_;

  // 确认与横幅
  ConfirmKind confirm_ = ConfirmKind::None;
  FreshHoldConfirm confirmState_;
  DashboardConfirmRequest confirmDashboard_;
  std::string activeInputControl_;
  bool suppressButtonShort_[3] = {false, false, false};
  bool suppressButtonLong_[3] = {false, false, false};
  bool suppressTouchTap_ = false;
  bool suppressTouchHold_ = false;
  bool suppressFrontShort_ = false;
  // 陈旧输入按整段手势丢弃：起始事件（Press/FrontPress/按键 Press）的判定
  // 沿用到同一手势的后续事件，避免丢了 Press 却执行其 Tap。
  int64_t inputResumeUs_ = 0;
  bool touchGestureStale_ = false;
  bool frontGestureStale_ = false;
  bool buttonGestureStale_[3] = {false, false, false};
  bool inputEventStale(int64_t timestampUs, int64_t nowUs) const;
  std::string banner_;
  int64_t bannerDeadlineMs_ = 0;
  std::string blePop_;
  bool bleProvisionWasActive_ = false;

  // 快照（无锁读；renderedSeq_ 跟踪已渲染代际）
  std::shared_ptr<const alert_core::AlertSnapshot> snapshot_;
  uint64_t snapshotSeq_ = 0;

  // 空闲感知刷新节奏
  RefreshPolicy refreshPolicy_;
  int64_t lastCriticalCheckMs_ = -100000;
  bool idleLogged_ = false;

  // 渲染节流
  bool dirty_ = true;
  bool forceFull_ = true;
  int64_t lastRenderMs_ = -10000;
  int64_t lastFullRenderMs_ = 0;
  int64_t lastScheduleCheckMs_ = -10000;

  // 电池/充电缓存
  eego::BatteryInfo batCache_{};
  int64_t lastBatMs_ = -100000;
  bool charging_ = false;
  int64_t lastChargeMs_ = -100000;

  // 跨任务标志（原子；notify* 只置位）
  std::atomic<uint32_t> netBits_{0};
  std::atomic<int> wifiState_{0};
  void* adminQueue_ = nullptr;  // QueueHandle_t：AdminEvent 小信箱

  // RTC 实例（app_task 私有；I2C 端口级锁保证与其他设备共享安全）
  eego::Pcf8563Rtc rtc_{I2C_NUM_0};

  // net 状态缓存（5s 有效期，避免每次渲染都过 net_task 命令队列）
  eego::net::NetStatusSnapshot netStatusCache_{};
  int64_t netStatusCacheMs_ = 0;

  size_t muteRuleCount_ = 0;

  // 渲染触发缓存
  int64_t renderedClockKey_ = -2;   // 已渲染状态栏的分钟键（-1 = 时间不可用）
  int64_t lastClockCheckMs_ = 0;
  int64_t lastBleProvRenderMs_ = 0;
  bool dashboardWasAvailable_ = false;
  std::string onboardingKey_;       // 引导页显示内容签名（地址/WiFi 变化即重绘）
  int64_t lastOnboardingCheckMs_ = 0;
  int64_t lastStatusRenderMs_ = 0;

  // 双击门控缓存
  std::string doublePressContext_;
  uint32_t doublePressRevision_ = 0;
  int64_t lastDoublePressSyncMs_ = -100000;
  bool doublePressSynced_ = false;

  // 非阻塞网络静默等待（休眠前）
  NetIdleWait sleepIdleWait_;
  bool sleepPending_ = false;
  int64_t sleepRequestedMs_ = 0;

  // 电源模式 / 电池阶段机状态
  bool mains_ = true;
  BattPhase battPhase_ = BattPhase::Mains;
  battery::WakeSource wakeSource_ = battery::WakeSource::PowerOn;
  bool fastWake_ = false;
  bool restartRequested_ = false;    // 快速路径中按键：重启进完整应用
  int64_t phaseStartMs_ = 0;
  bool batchStarted_ = false;        // 本次唤醒的取数批次已发起
  bool wifiSeen_ = false;            // 本次唤醒 WiFi 曾连上
  uint64_t wakeStartUnix_ = 0;
  bool displayedThisBoot_ = false;   // 本次启动已刷过屏（控制器 DTM1 = 面板内容）
  int64_t wakePressGuardUntilMs_ = 0;  // 按键唤醒：唤醒那一下不当作操作
  int64_t lastInputMs_ = 0;
  bool fallbackArmed_ = false;
};

}  // namespace panel
