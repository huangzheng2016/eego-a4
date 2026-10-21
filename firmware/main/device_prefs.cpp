// SPDX-License-Identifier: GPL-3.0-or-later
#include "device_prefs.h"

#include <cstring>

#include "nvs.h"

namespace panel {

DevicePrefs& DevicePrefs::instance() {
  static DevicePrefs instance;
  return instance;
}

void DevicePrefs::load() {
  mutes_.clear();
  nvs_handle_t handle;
  if (nvs_open("ui", NVS_READONLY, &handle) != ESP_OK) return;
  size_t length = 0;
  if (nvs_get_str(handle, "mute", nullptr, &length) == ESP_OK && length > 1 &&
      length <= 2048) {
    char buffer[2048];
    if (nvs_get_str(handle, "mute", buffer, &length) == ESP_OK) {
      char* save = nullptr;
      for (char* token = strtok_r(buffer, ",", &save); token != nullptr;
           token = strtok_r(nullptr, ",", &save)) {
        if (strlen(token) == 40 && mutes_.size() < 32) mutes_.emplace_back(token);
      }
    }
  }
  nvs_close(handle);
}

void DevicePrefs::save() {
  nvs_handle_t handle;
  if (nvs_open("ui", NVS_READWRITE, &handle) != ESP_OK) return;
  std::string joined;
  for (size_t i = 0; i < mutes_.size(); i++) {
    if (i != 0) joined += ',';
    joined += mutes_[i];
  }
  nvs_set_str(handle, "mute", joined.c_str());
  nvs_commit(handle);
  nvs_close(handle);
}

}  // namespace panel
