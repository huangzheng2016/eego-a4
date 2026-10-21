// SPDX-License-Identifier: GPL-3.0-or-later
// 设备本地偏好实现（NVS "ui" 命名空间；见 device_prefs.h）。
#include "device_prefs.h"

#include <cstring>

#include "nvs.h"
#include "nvs_flash.h"

namespace panel {

namespace {

constexpr char kNvsNs[] = "ui";
constexpr char kKeyLang[] = "lang";
constexpr char kKeyPmode[] = "pmode";
constexpr char kKeyBPoll[] = "bpoll";
constexpr char kKeyMute[] = "mute";
constexpr uint32_t kBPollMinS = 60;
constexpr uint32_t kBPollMaxS = 21600;  // 6h
constexpr size_t kMaxMutes = 32;
constexpr size_t kFingerprintLen = 40;  // sha1 hex

}  // namespace

DevicePrefs& DevicePrefs::instance() {
  static DevicePrefs inst;
  return inst;
}

void DevicePrefs::load() {
  nvs_handle_t h;
  if (nvs_open(kNvsNs, NVS_READONLY, &h) != ESP_OK) {
    return;  // 命名空间不存在 = 首次运行，保持默认
  }
  uint8_t u8 = 0;
  if (nvs_get_u8(h, kKeyLang, &u8) == ESP_OK) setLang(u8 == 1 ? Lang::En : Lang::Zh);
  if (nvs_get_u8(h, kKeyPmode, &u8) == ESP_OK && u8 <= 2) {
    pmode_ = static_cast<PowerMode>(u8);
  }
  uint32_t u32 = 0;
  if (nvs_get_u32(h, kKeyBPoll, &u32) == ESP_OK && u32 >= kBPollMinS && u32 <= kBPollMaxS) {
    bpoll_s_ = u32;
  }
  size_t len = 0;
  if (nvs_get_str(h, kKeyMute, nullptr, &len) == ESP_OK && len > 1 && len <= 2048) {
    char buf[2048];
    if (nvs_get_str(h, kKeyMute, buf, &len) == ESP_OK) {
      mutes_.clear();
      char* save = nullptr;
      for (char* tok = strtok_r(buf, ",", &save); tok != nullptr;
           tok = strtok_r(nullptr, ",", &save)) {
        if (strlen(tok) == kFingerprintLen && mutes_.size() < kMaxMutes) {
          mutes_.emplace_back(tok);
        }
      }
    }
  }
  nvs_close(h);
}

void DevicePrefs::save() {
  nvs_handle_t h;
  if (nvs_open(kNvsNs, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_u8(h, kKeyLang, static_cast<uint8_t>(lang_));
  nvs_set_u8(h, kKeyPmode, static_cast<uint8_t>(pmode_));
  nvs_set_u32(h, kKeyBPoll, bpoll_s_);
  // 指纹表序列化为逗号分隔串（40hex×32 + 逗号 ≈ 1311B，NVS str 上限内）
  std::string joined;
  for (size_t i = 0; i < mutes_.size(); i++) {
    if (i != 0) joined += ',';
    joined += mutes_[i];
  }
  nvs_set_str(h, kKeyMute, joined.c_str());
  nvs_commit(h);
  nvs_close(h);
}

void DevicePrefs::setBatteryPollS(uint32_t s) {
  if (s < kBPollMinS) s = kBPollMinS;
  if (s > kBPollMaxS) s = kBPollMaxS;
  bpoll_s_ = s;
}

bool DevicePrefs::isMuted(const std::string& fingerprint) const {
  for (const std::string& fp : mutes_) {
    if (fp == fingerprint) return true;
  }
  return false;
}

bool DevicePrefs::addMute(const std::string& fingerprint) {
  if (fingerprint.size() != kFingerprintLen) return false;
  if (isMuted(fingerprint)) return false;
  if (mutes_.size() >= kMaxMutes) return false;
  mutes_.push_back(fingerprint);
  return true;
}

}  // namespace panel
