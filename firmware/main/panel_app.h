// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// panel_app —— 告警/数据面板应用状态机（FW5/M18 + DP4/M26 组合根的 UI 属主）。
//
// 并发模型（M12 定稿，本类强制执行）：
//   * 全部渲染/导航/本地动作只在 app_task 上下文（begin/service/handle* 由
//     app_main 主循环调用）；触控 8ms 轮询与按键队列在 app_main 泵入；
//   * 告警数据只读：SnapshotPublisher::acquire 无锁取不可变快照，绝不直写
//     AlertStore；告警动作（ack/ackAll/clear）与静音规则（set/clear/query）
//     一律经 NetStack 命令队列由 net_task 执行；本类不触碰 HttpPool/适配器；
//   * 跨任务入口只有 notifyNetEvent / notifyAdminEvent：只做原子置位与小
//     信箱投递，短平快，绝不做渲染/IO。
//
// 电源双策略（M13/M18）：
//   * 常供电：WiFi modem-sleep（setPowerSave(true)）常驻，http_push 即时可达；
//   * 电池：唤醒 → 重连 → 全源轮询 → 渲染 → 短暂交互窗口 → 安全深睡
//     （GPIO4 锁存保持 + 定时唤醒 + 电源键 EXT0 唤醒）；唤醒=重启，循环。
//   模式判定：DeviceSettings Auto = 充电状态（无电池读数回退常供电），S4 可强制。
//
// 静音（M21 store 级）：详情静音 = 指纹规则经 net_task 落入 AlertStore
// （重估 muted 标志 + 快照发布），并持久化到 config.json mute_rules（重启
// 恢复）；S4 清除全部静音 = 清空规则表。总览/列表/状态统计统一按记录的
// muted 标志过滤（store 语义），不再有 UI 级指纹过滤。

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "alert_core/snapshot.h"
#include "alert_core/type_registry.h"
#include "board/board_boot.h"
#include "dashboard_runtime.h"
#include "device_settings/device_settings.h"
#include "eego_hal.h"
#include "eego_ui.h"
#include "net_stack/source_health.h"
#include "panel_render.h"
#include "panel_types.h"
#include "ui_text.h"

// 仅声明引用所需（定义见 alert_core/store.h，panel_app.cpp 经 net_stack 获得）
namespace alert_core {
struct MuteRule;
}  // namespace alert_core

namespace panel {

// 二次确认状态机契约：普通动作接受同控件再次激活；网络恢复、WiFi 重置和
// 恢复出厂只接受实体电源键的第二次确认。任何其他输入取消，窗口为 5s。

class PanelApp {
 public:
  struct Deps {
    eego::Uc8279cDisplay* display = nullptr;
    eego::Gslx680Touch* touch = nullptr;
    eego::PortraitCanvas* canvas = nullptr;
    eego::board::BootStatus boot;
    alert_core::SnapshotPublisher* publisher = nullptr;  // 必须 outlive PanelApp
    alert_core::TypeRegistry* registry = nullptr;        // 必须 outlive PanelApp
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

  // 跨任务通知（必须短平快；可在 net_task/httpd/sys_evt 上下文调用）。
  void notifyNetEvent(int wifiState, bool sourceHealthChanged);
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
  void onButtonShort(eego::ButtonId id);
  void onButtonLong(eego::ButtonId id);
  void onFrontKeyShort();
  void onFrontKeyLong();

  // ---- 导航 ----
  void gotoPage(Page p);          // 切页（下一帧强制 Full）
  void back();                    // 电源键长按/返回软键
  void moveFocus(int delta);      // S0/S2/S3 软键聚焦、S1 行选择、S4 行聚焦
  void activateFocused();         // 电源键短按
  void activateSoftKey(size_t idx);
  void activateSettingRow(size_t row);  // S4 全局行号，按 8 行分页显示

  // ---- 动作（NetStack / DeviceSettings / recovery）----
  void doAck(const std::string& id);
  void doAckAll();
  void doClear(const std::string& id);
  void doMute(const alert_core::AlertRecord& rec);
  void doClearMutes();
  void persistMuteRules(const std::vector<alert_core::MuteRule>& rules);
  void refreshMuteRuleCount();
  void doPollNow();
  void doOpenRecovery();
  void doWifiReset();
  void doFactoryReset();
  bool updateSettings(const eego::settings::Settings& candidate);
  void applySettings(uint32_t changed);
  static void onSettingsChanged(const eego::settings::Settings& values, uint32_t changed,
                                void* ctx);
  uint64_t nextWakeAt(int64_t nowMs) const;
  void armConfirm(ConfirmKind kind);
  void executeArmedConfirm();
  void clearConfirm();
  void showBanner(const char* text, uint32_t ms);
  void showBannerFmt(uint32_t ms, const char* fmt, ...)
      __attribute__((format(printf, 3, 4)));

  // ---- 电源模式 ----
  bool computeMains();
  void onModeChange(bool mains);
  void batteryFsm(int64_t nowMs);
  // durationMs < 0 使用电池轮询周期；计划睡眠传入明确的窗口剩余时长。
  void enterSleep(int64_t nowMs, int64_t durationMs = -1);

  // ---- 渲染 ----
  void markDirty(bool forceFull = false) {
    dirty_ = true;
    if (forceFull) forceFull_ = true;
  }
  void render(int64_t nowMs);
  void buildStatusBar(StatusBarModel& m);
  void buildS0(S0Model& m);
  void buildS1(S1Model& m);
  void buildS2(S2Model& m);
  void buildS3(S3Model& m);
  void buildS4(S4Model& m);

  // ---- 数据辅助 ----
  uint64_t nowUnix();             // 统一时间源（TimeSource：SNTP 系统时间 > RTC > 0）
  void refreshBattery(int64_t nowMs);
  const eego::net::NetStatusSnapshot& netStatus();  // 5s 缓存的源/WiFi/时间状态
  const alert_core::AlertRecord* findRecord(const std::string& id) const;
  bool recordVisible(const alert_core::AlertRecord& r) const;  // 非 store 静音
  size_t visibleRecords(std::vector<const alert_core::AlertRecord*>& out) const;
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
  int wakeCause_ = 0;

  // 页面/导航状态（仅 app_task 上下文读写）
  Page page_ = Page::Overview;
  size_t focus_ = 0;
  size_t tablePage_ = 0;
  std::string detailId_;
  bool provisionSucceeded_ = false;

  // 确认与横幅
  ConfirmKind confirm_ = ConfirmKind::None;
  int64_t confirmDeadlineMs_ = 0;
  std::string banner_;
  int64_t bannerDeadlineMs_ = 0;

  // 快照（无锁读；renderedSeq_ 跟踪已渲染代际）
  std::shared_ptr<const alert_core::AlertSnapshot> snapshot_;
  uint64_t snapshotSeq_ = 0;

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

  // 电源模式状态
  bool mains_ = true;
  BattPhase battPhase_ = BattPhase::Mains;
  int64_t cycleStartMs_ = 0;
  int64_t pollDeadlineMs_ = 0;
  bool batteryPollStarted_ = false;
  int64_t awakeDeadlineMs_ = 0;
  int64_t lastInputMs_ = 0;
  bool fallbackArmed_ = false;
};

}  // namespace panel
