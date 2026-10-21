// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

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

class DashboardRuntime {
 public:
  struct Deps {
    eego::PortraitCanvas* canvas = nullptr;
    eego::net::NetStack* net = nullptr;
    alert_core::SnapshotPublisher* alerts = nullptr;
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
  void activate();
  void touch(int16_t x, int16_t y);
  bool back();
  bool consumeExitRequest();
  bool gotoPage(const std::string& page_id);
  bool gotoPanel(const std::string& panel_id);
  std::string linkedPanelForAlert(const alert_core::AlertRecord& record) const;

  void diag(const char* args);
  bool smoke();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace panel
