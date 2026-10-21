// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

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

  void service(bool mains);
  void pollBattery();
  bool networkIdle() const;
  void setMains(bool mains);
  void invalidateAll();
  bool needsRender() const;
  void render();

  void moveFocus(int delta, bool adjust);
  void movePage(int delta);
  void refreshCurrent();
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

  void diag(const char* args);
  bool smoke();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace panel
