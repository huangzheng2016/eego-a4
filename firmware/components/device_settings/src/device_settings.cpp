// SPDX-License-Identifier: GPL-3.0-or-later
#include "device_settings/device_settings.h"

#include <cstdio>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace eego::settings {

namespace {

constexpr char kTag[] = "settings";
constexpr char kNvsNamespace[] = "device";
constexpr char kNvsRecordKey[] = "prefs";
constexpr size_t kMaxRecordBytes = 4000;

esp_err_t ensureNvs() {
  const esp_err_t err = nvs_flash_init();
  return err == ESP_ERR_INVALID_STATE ? ESP_OK : err;
}

esp_err_t saveRecord(const Settings& values, uint32_t revision, std::string& error) {
  esp_err_t err = ensureNvs();
  if (err != ESP_OK) {
    error = "NVS is unavailable";
    return err;
  }
  nvs_handle_t handle;
  err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    error = "cannot open device settings namespace";
    return err;
  }
  const std::string text = toJson(values).toString();
  if (text.size() >= kMaxRecordBytes) {
    nvs_close(handle);
    error = "settings record exceeds NVS budget";
    return ESP_ERR_INVALID_SIZE;
  }
  err = nvs_set_str(handle, kNvsRecordKey, text.c_str());
  if (err == ESP_OK) err = nvs_set_u32(handle, "revision", revision);
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  if (err != ESP_OK) error = "NVS settings commit failed";
  return err;
}

bool loadRecord(Settings& values, uint32_t& revision) {
  nvs_handle_t handle;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
  size_t length = 0;
  bool ok = nvs_get_str(handle, kNvsRecordKey, nullptr, &length) == ESP_OK && length > 1 &&
            length <= kMaxRecordBytes;
  std::string text;
  if (ok) {
    text.resize(length);
    ok = nvs_get_str(handle, kNvsRecordKey, text.data(), &length) == ESP_OK;
    if (ok && !text.empty() && text.back() == '\0') text.pop_back();
  }
  uint32_t stored_revision = 0;
  if (ok) ok = nvs_get_u32(handle, "revision", &stored_revision) == ESP_OK;
  nvs_close(handle);
  if (!ok) return false;

  alert_core::JsonValue root;
  alert_core::JsonParseError parse_error;
  if (!alert_core::jsonParse(text, root, parse_error)) return false;
  const alert_core::JsonValue* version = root.find("schema_version");
  if (version == nullptr || version->type() != alert_core::JsonValue::Type::Int ||
      version->intValue() != kSchemaVersion) {
    ESP_LOGW(kTag, "unsupported settings schema; using defaults");
    return false;
  }
  Settings parsed;
  FieldErrors errors;
  if (!mergePatch(Settings{}, root, parsed, errors)) {
    ESP_LOGW(kTag, "invalid settings record; using defaults");
    return false;
  }
  values = parsed;
  revision = stored_revision;
  return true;
}

}  // namespace

DeviceSettings& DeviceSettings::instance() {
  static DeviceSettings instance;
  return instance;
}

void DeviceSettings::ensureMutex() const {
  if (mutex_ == nullptr) mutex_ = xSemaphoreCreateMutex();
}

esp_err_t DeviceSettings::init() {
  ensureMutex();
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  if (loaded_) {
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return ESP_OK;
  }
  const esp_err_t nvs_err = ensureNvs();
  if (nvs_err != ESP_OK) {
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return nvs_err;
  }

  Settings loaded_values;
  uint32_t loaded_revision = 0;
  if (loadRecord(loaded_values, loaded_revision)) {
    values_ = loaded_values;
    revision_ = loaded_revision;
    loaded_ = true;
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return ESP_OK;
  }

  values_ = Settings{};
  revision_ = 1;

  std::string error;
  const esp_err_t err = saveRecord(values_, revision_, error);
  if (err == ESP_OK) {
    loaded_ = true;
    ESP_LOGI(kTag, "device settings initialized with defaults");
  } else {
    ESP_LOGE(kTag, "settings defaults persist failed: %s", error.c_str());
  }
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  return err;
}

Snapshot DeviceSettings::snapshot() const {
  ensureMutex();
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  Snapshot out{values_, capabilities_, revision_, loaded_};
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  return out;
}

void DeviceSettings::setCapabilities(const Capabilities& capabilities) {
  ensureMutex();
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  capabilities_ = capabilities;
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
}

bool DeviceSettings::update(const Settings& candidate, uint32_t* changed, std::string& error) {
  if (changed != nullptr) *changed = 0;
  FieldErrors errors;
  validate(candidate, errors);
  if (!errors.empty()) {
    error = errors.front().field + ": " + errors.front().message;
    return false;
  }

  ensureMutex();
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  const uint32_t mask = changedFields(values_, candidate);
  if (mask == 0) {
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return true;
  }
  // Judge frontlight availability by the candidate's hardware override, not by
  // capabilities_.frontlight: that is only refreshed by the change callback
  // after this update, so it is stale when the override changes together with
  // the frontlight fields.
  bool frontlight_allowed = capabilities_.frontlight_chip;
  if (candidate.frontlight_hardware == FrontlightHardware::Present) frontlight_allowed = true;
  if (candidate.frontlight_hardware == FrontlightHardware::Absent) frontlight_allowed = false;
  if ((mask & kChangedFrontlight) != 0 && !frontlight_allowed) {
    error = "frontlight hardware is unavailable";
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return false;
  }
  if (candidate.ble_observer_enabled && !capabilities_.ble_observer) {
    error = "BLE observer is unavailable in this firmware";
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return false;
  }
  uint32_t next_revision = revision_ + 1;
  if (next_revision == 0) next_revision = 1;
  if (saveRecord(candidate, next_revision, error) != ESP_OK) {
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return false;
  }
  values_ = candidate;
  revision_ = next_revision;
  loaded_ = true;
  Slot notifications[6];
  for (size_t i = 0; i < 6; i++) notifications[i] = slots_[i];
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));

  if (changed != nullptr) *changed = mask;
  for (const Slot& slot : notifications) {
    if (slot.callback != nullptr) slot.callback(candidate, mask, slot.ctx);
  }
  return true;
}

int DeviceSettings::subscribe(Callback callback, void* ctx) {
  if (callback == nullptr) return -1;
  ensureMutex();
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  int id = -1;
  for (Slot& slot : slots_) {
    if (slot.callback == nullptr) {
      id = next_subscription_++;
      if (next_subscription_ <= 0) next_subscription_ = 1;
      slot = Slot{id, callback, ctx};
      break;
    }
  }
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  return id;
}

esp_err_t DeviceSettings::erasePersisted() {
  esp_err_t err = ensureNvs();
  if (err != ESP_OK) return err;
  nvs_handle_t handle;
  err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
  if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
  if (err != ESP_OK) return err;
  err = nvs_erase_all(handle);
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  return err;
}

}  // namespace eego::settings
