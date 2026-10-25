// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "dashboard_core/model.h"

namespace alert_core {
struct AlertRecord;
class SnapshotPublisher;
}  // namespace alert_core

namespace eego {
class PortraitCanvas;
namespace net {
class NetStack;
}
}  // namespace eego

namespace panel {

struct DashboardConfirmRequest {
  std::string panel_id;
  std::string action_id;
  std::string resolved_target;
  std::string resolved_value;
  std::string target_identity;
  uint64_t target_generation = 0;
  uint64_t page_epoch = 0;
  uint64_t snapshot_epoch = 0;
};

// 告警面板/横幅里的一条告警（dashboard 事件视图：规则派生事件 + 外部推送的
// alert_core 记录）。供设备端告警详情页逐条浏览与操作。
struct DashboardAlertItem {
  std::string identity;        // dashboardEventIdentity（source_id + "\n" + event id）
  std::string core_id;         // alert_core 记录 id（外部告警）；规则派生事件为空
  std::string title;
  std::string message;
  std::string source_id;
  std::string rule_id;
  std::string linked_panel_id;  // 可跳转的趋势面板（空 = 无）
  uint8_t severity = 0;         // 0 info / 1 warning / 2 critical
  bool active = true;           // false = 已恢复/清除
  bool acknowledged = false;    // 已确认（本代际不再进横幅/标记）
  int64_t timestamp_ms = 0;     // unix 毫秒（0 = 未知）
};

// dashboard 数据源运行状态（设备状态页）。
struct DashboardSourceInfo {
  std::string id;
  bool enabled = true;
  bool has_data = false;          // 有缓存或本次运行成功过的结果
  bool offline = false;
  bool stale = true;
  uint32_t consecutive_failures = 0;
  int last_http_status = 0;
  int64_t last_success_ms = 0;    // unix 毫秒（0 = 从未）
  int64_t last_attempt_ms = 0;
  std::string error;              // 已脱敏的最近错误（空 = 无）
};

class DashboardRuntime {
 public:
  struct Deps {
    eego::PortraitCanvas* canvas = nullptr;
    eego::net::NetStack* net = nullptr;
    alert_core::SnapshotPublisher* alerts = nullptr;
    std::function<void(const DashboardConfirmRequest&)> confirm_action;
    std::string config_path = "/sdcard/alertpanel/dashboard.json";
    std::string cache_dir = "/sdcard/alertpanel/dashboard_cache";
  };

  DashboardRuntime();
  ~DashboardRuntime();
  DashboardRuntime(const DashboardRuntime&) = delete;
  DashboardRuntime& operator=(const DashboardRuntime&) = delete;

  bool begin(const Deps& deps);
  bool reload();
  bool available() const;
  // 最近一次加载失败原因（未配置时为空或 "dashboard deleted" 等）。
  std::string unavailableReason() const;
  std::string title() const;

  void service(bool mains);
  // 电池唤醒：一次性拉取全部到期数据源，budget_ms 后未完成的请求被取消。
  void pollBattery(int64_t budget_ms);
  // 结束电池取数批次（取消在途请求；深睡前调用）。
  void cancelBatteryCycle();
  bool networkIdle() const;
  // 电池深睡帧的状态行文字（替代常规的页码/时钟/数据源计数；告警横幅仍优先）。
  // 空串恢复常规状态行。
  void setBatteryStatus(const std::string& text);
  // 只渲染进 framebuffer、不刷屏（调用方随后自行哈希/决定刷新方式）。
  void renderOffscreen();
  void setMains(bool mains);
  void invalidateAll();
  bool needsRender() const;
  void render();

  void moveFocus(int delta, bool adjust);
  void movePage(int delta);
  // 上/下一屏：先在页面子屏（page_index）间移动，再跨页（循环）。
  void moveScreen(int delta);
  // 主页：默认页第一屏，清空返回栈与面板焦点。
  void home();
  bool atHome() const;
  bool firstScreen() const;
  void refreshCurrent();
  void refreshAll();
  // 全局刷新周期覆盖（DeviceSettings.data_refresh_s；0 = 按面板/数据源配置）。
  // 用户在面板上单独设定的周期优先。
  void setGlobalRefresh(uint32_t refresh_ms);
  void activate();
  void touch(int16_t x, int16_t y);
  bool back();
  bool consumeExitRequest();
  bool gotoPage(const std::string& page_id);
  bool gotoPanel(const std::string& panel_id);
  bool focusAction(const std::string& panel_id, const std::string& action_id,
                   const std::string& event_id = {});
  bool executeActionRef(const std::string& panel_id, const std::string& action_id,
                        eego::dashboard::ActionKind* kind = nullptr);
  bool executeConfirmed(const DashboardConfirmRequest& request);
  uint64_t pageEpoch() const;
  uint64_t snapshotEpoch() const;
  std::string pageContext() const;
  std::string focusedControl() const;
  std::string touchControl(int16_t x, int16_t y) const;
  void showStatusMessage(const std::string& text, uint32_t duration_ms);
  std::string linkedPanelForAlert(const alert_core::AlertRecord& record) const;

  // 告警详情：点按告警面板行/横幅或在告警面板上按 Power 时登记请求，
  // PanelApp 取走后切到详情页。panel_id 为空 = 全部告警（横幅入口）。
  bool consumeDetailRequest(std::string& panel_id, std::string& identity);
  std::vector<DashboardAlertItem> alertItems(const std::string& panel_id) const;
  bool acknowledgeEvent(const std::string& identity);
  // 当前活动、未确认的严重（critical）事件身份（空闲刷新的“新严重告警立即刷新”判定）。
  std::vector<std::string> activeCriticalIdentities() const;
  // web 端应用/删除 dashboard 后置位一次（取走即清）；PanelApp 视为用户交互。
  bool consumeConfigChanged();
  std::vector<DashboardSourceInfo> sources() const;

  void diag(const char* args);
  bool smoke();

  // Offscreen preview of a candidate dashboard (admin render-preview). app_task
  // only. Renders page/screen of `candidate` into `canvas` (which must wrap an
  // offscreen display) with the device layout; values come from the candidate's
  // samples/events, else live data of same-id sources, else placeholders.
  struct PreviewInfo {
    bool bad_request = false;
    std::string error;
    std::string page_id;
    size_t screen = 0;
    size_t screen_count = 0;
    std::string data;  // "bundle" | "live" | "mixed" | "placeholder" | "none"
  };
  bool renderPreview(eego::PortraitCanvas& canvas, const eego::dashboard::Dashboard& candidate,
                     const std::string& page_id, size_t screen, PreviewInfo& info) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace panel
