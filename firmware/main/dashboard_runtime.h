// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "dashboard_core/model.h"
#include "device_status.h"

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

struct DashboardAlertItem {
  std::string identity;
  std::string core_id;
  std::string title;
  std::string message;
  std::string source_id;
  std::string rule_id;
  std::string linked_panel_id;
  uint8_t severity = 0;         // 0 info / 1 warning / 2 critical
  bool active = true;
  bool acknowledged = false;
  int64_t timestamp_ms = 0;
};

struct DashboardSourceInfo {
  std::string id;
  bool enabled = true;
  bool has_data = false;
  bool offline = false;
  bool stale = true;
  uint32_t consecutive_failures = 0;
  int last_http_status = 0;
  int64_t last_success_ms = 0;
  int64_t last_attempt_ms = 0;
  std::string error;
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
  std::string unavailableReason() const;
  std::string title() const;

  void service(bool mains);
  void pollBattery(int64_t budget_ms);
  void cancelBatteryCycle();
  bool networkIdle() const;
  void setDeviceState(const DeviceStateInfo& info, int tz_offset_min);
  void setSleepFrame(bool enabled, uint64_t stamp_unix = 0, int battery_pct = -1);
  void renderOffscreen();
  void setMains(bool mains);
  void invalidateAll();
  bool needsRender() const;
  void render();

  void moveFocus(int delta, bool adjust);
  void movePage(int delta);
  void moveScreen(int delta);
  bool showScreen(size_t index);
  size_t screenIndex() const;
  size_t screenCount() const;
  std::string screenPageId() const;
  void home();
  bool atHome() const;
  bool firstScreen() const;
  void refreshCurrent();
  void refreshAll();
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

  bool consumeDetailRequest(std::string& panel_id, std::string& identity);
  std::vector<DashboardAlertItem> alertItems(const std::string& panel_id) const;
  bool acknowledgeEvent(const std::string& identity);
  std::vector<std::string> activeCriticalIdentities() const;
  bool consumeConfigChanged();
  std::vector<DashboardSourceInfo> sources() const;

  void diag(const char* args);
  bool smoke();

  // Offscreen preview of a candidate dashboard (admin render-preview). app_task
  // only. Renders one page (= screen) of `candidate` into `canvas` (which must wrap an
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
                     const std::string& page_id, PreviewInfo& info) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace panel
