// SPDX-License-Identifier: GPL-3.0-or-later
#include "battery_mode.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "atomic_file.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "eego_pins.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_card.h"

namespace panel::battery {
namespace {
const char* TAG = "battery";

RTC_DATA_ATTR RtcState g_rtc;
bool g_rtc_checked = false;

struct PsramBuffer {
  uint8_t* data = nullptr;
  explicit PsramBuffer(size_t size) {
    data = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
    if (data == nullptr) data = static_cast<uint8_t*>(malloc(size));
  }
  ~PsramBuffer() { free(data); }
  PsramBuffer(const PsramBuffer&) = delete;
  PsramBuffer& operator=(const PsramBuffer&) = delete;
};

}  // namespace

WakeSource wakeSource() {
  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_UNDEFINED:
      return WakeSource::PowerOn;
    case ESP_SLEEP_WAKEUP_TIMER:
      return WakeSource::Timer;
    case ESP_SLEEP_WAKEUP_EXT0:
      return WakeSource::PowerKey;
    case ESP_SLEEP_WAKEUP_EXT1:
      return WakeSource::Buttons;
    default:
      return WakeSource::Other;
  }
}

const char* wakeSourceName(WakeSource source) {
  switch (source) {
    case WakeSource::PowerOn: return "power-on";
    case WakeSource::Timer: return "timer";
    case WakeSource::PowerKey: return "power-key";
    case WakeSource::Buttons: return "buttons";
    default: return "other";
  }
}

RtcState& rtcState() {
  if (!g_rtc_checked) {
    g_rtc_checked = true;
    if (!rtcValid(g_rtc)) {
      ESP_LOGI(TAG, "RTC state invalid or first boot: reset");
      rtcReset(g_rtc);
    }
  }
  return g_rtc;
}

void commitRtcState() { rtcSeal(rtcState()); }

bool loadStoredFrame(uint8_t* out, size_t size, uint64_t expected_hash) {
  if (out == nullptr || !eego::SdCard::instance().ready()) return false;
  eego::fsutil::recoverFile(kFramePath);
  FILE* f = fopen(kFramePath, "rb");
  if (f == nullptr) return false;
  bool ok = false;
  if (fseek(f, 0, SEEK_END) == 0) {
    const long length = ftell(f);
    if (length > 0 && static_cast<size_t>(length) <= frameFileMaxSize(size) &&
        fseek(f, 0, SEEK_SET) == 0) {
      PsramBuffer file(static_cast<size_t>(length));
      if (file.data != nullptr &&
          fread(file.data, 1, static_cast<size_t>(length), f) == static_cast<size_t>(length)) {
        uint64_t hash = 0;
        ok = decodeFrameFile(file.data, static_cast<size_t>(length), out, size, hash) &&
             hash == expected_hash;
        if (!ok) ESP_LOGW(TAG, "stored frame corrupt or stale (hash mismatch)");
      }
    }
  }
  fclose(f);
  return ok;
}

bool storeFrame(const uint8_t* frame, size_t size, uint64_t hash) {
  if (frame == nullptr || !eego::SdCard::instance().ready()) return false;
  const size_t cap = frameFileMaxSize(size);
  PsramBuffer file(cap);
  if (file.data == nullptr) return false;
  const size_t length = encodeFrameFileTo(frame, size, hash, file.data, cap);
  if (length == 0) return false;
  const bool ok = eego::fsutil::atomicWriteFile(kFramePath, file.data, length);
  ESP_LOGI(TAG, "stored frame %u bytes (%s)", static_cast<unsigned>(length), ok ? "ok" : "FAILED");
  return ok;
}

void deepSleep(uint64_t sleep_s, bool buttons) {
  gpio_hold_en(static_cast<gpio_num_t>(eego::POWER_LATCH));
  gpio_deep_sleep_hold_en();

  if (sleep_s > 0) esp_sleep_enable_timer_wakeup(sleep_s * 1000000ULL);

  const gpio_num_t power = static_cast<gpio_num_t>(eego::BUTTON_POWER);
  rtc_gpio_pullup_dis(power);
  rtc_gpio_pulldown_en(power);
  esp_sleep_enable_ext0_wakeup(power, 1);

  if (buttons) {
    const gpio_num_t up = static_cast<gpio_num_t>(eego::BUTTON_UP);
    const gpio_num_t down = static_cast<gpio_num_t>(eego::BUTTON_DOWN);
    rtc_gpio_pulldown_dis(up);
    rtc_gpio_pullup_en(up);
    rtc_gpio_pulldown_dis(down);
    rtc_gpio_pullup_en(down);
    const uint64_t mask = (1ULL << eego::BUTTON_UP) | (1ULL << eego::BUTTON_DOWN);
    esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
  }
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

  ESP_LOGI(TAG, "deep sleep: timer=%llus power-key%s", static_cast<unsigned long long>(sleep_s),
           buttons ? " up/down" : "");
  esp_deep_sleep_start();
  __builtin_unreachable();
}

void powerOff() {
  const gpio_num_t power = static_cast<gpio_num_t>(eego::BUTTON_POWER);
  for (int i = 0; i < 50 && gpio_get_level(power) != 0; ++i) vTaskDelay(pdMS_TO_TICKS(100));

  const gpio_num_t latch = static_cast<gpio_num_t>(eego::POWER_LATCH);
  ESP_LOGI(TAG, "power off: releasing power latch");
  gpio_hold_dis(latch);
  gpio_set_level(latch, 0);
  vTaskDelay(pdMS_TO_TICKS(1500));

  ESP_LOGW(TAG, "still powered (external supply): deep sleep until the power key");
  deepSleep(0, false);
}

}  // namespace panel::battery
