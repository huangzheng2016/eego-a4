// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Legacy NVS "ui" mute fingerprints. All actual device preferences moved to
// device_settings; this class only supports the pre-M21 store-rule migration.

#include <string>
#include <vector>

namespace panel {

class DevicePrefs {
 public:
  static DevicePrefs& instance();

  void load();
  void save();
  const std::vector<std::string>& mutes() const { return mutes_; }
  void clearMutes() { mutes_.clear(); }

 private:
  DevicePrefs() = default;
  std::vector<std::string> mutes_;
};

}  // namespace panel
