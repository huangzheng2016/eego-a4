// SPDX-License-Identifier: GPL-3.0-or-later
#include "admin/input_bindings.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace eego::admin {

namespace {

constexpr char kTag[] = "input_bindings";
constexpr char kNamespace[] = "input";
constexpr char kRecordKey[] = "bindings";
constexpr size_t kMaxRecordBytes = 4000;

esp_err_t ensureNvs() {
  const esp_err_t err = nvs_flash_init();
  return err == ESP_ERR_INVALID_STATE ? ESP_OK : err;
}

esp_err_t saveRecord(const InputBindingSet& values, std::string& error) {
  esp_err_t err = ensureNvs();
  if (err != ESP_OK) {
    error = "NVS is unavailable";
    return err;
  }
  const std::string text = inputBindingsToJson(values).toString();
  if (text.size() + 1 > kMaxRecordBytes) {
    error = "input binding record exceeds NVS budget";
    return ESP_ERR_INVALID_SIZE;
  }
  nvs_handle_t handle;
  err = nvs_open(kNamespace, NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    error = "cannot open input binding namespace";
    return err;
  }
  err = nvs_set_str(handle, kRecordKey, text.c_str());
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  if (err != ESP_OK) error = "input binding NVS commit failed";
  return err;
}

enum class LoadResult { Missing, Valid, Corrupt };

LoadResult loadRecord(InputBindingSet& out) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(kNamespace, NVS_READONLY, &handle);
  if (err == ESP_ERR_NVS_NOT_FOUND) return LoadResult::Missing;
  if (err != ESP_OK) return LoadResult::Corrupt;
  size_t length = 0;
  err = nvs_get_str(handle, kRecordKey, nullptr, &length);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    return LoadResult::Missing;
  }
  if (err != ESP_OK || length <= 1 || length > kMaxRecordBytes) {
    nvs_close(handle);
    return LoadResult::Corrupt;
  }
  std::string text(length, '\0');
  err = nvs_get_str(handle, kRecordKey, text.data(), &length);
  nvs_close(handle);
  if (err != ESP_OK) return LoadResult::Corrupt;
  if (!text.empty() && text.back() == '\0') text.pop_back();
  alert_core::JsonValue root;
  alert_core::JsonParseError parse_error;
  if (!alert_core::jsonParse(text, root, parse_error)) return LoadResult::Corrupt;
  InputBindingSet candidate;
  InputBindingErrors errors;
  if (!inputBindingsFromJson(root, true, candidate, errors)) return LoadResult::Corrupt;
  candidate.loaded = true;
  out = std::move(candidate);
  return LoadResult::Valid;
}

const dashboard::Dashboard* usableDashboard(const dashboard::Dashboard* dashboard) {
  return dashboard != nullptr && !dashboard->pages.empty() ? dashboard : nullptr;
}

}  // namespace

InputBindings& InputBindings::instance() {
  static InputBindings instance;
  return instance;
}

void InputBindings::ensureMutex() const {
  if (mutex_ == nullptr) mutex_ = xSemaphoreCreateMutex();
}

esp_err_t InputBindings::init() {
  ensureMutex();
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  if (initialized_) {
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return ESP_OK;
  }
  initialized_ = true;
  const esp_err_t nvs_err = ensureNvs();
  if (nvs_err != ESP_OK) {
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return nvs_err;
  }

  InputBindingSet loaded;
  const LoadResult result = loadRecord(loaded);
  if (result == LoadResult::Valid) {
    values_ = std::move(loaded);
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return ESP_OK;
  }

  values_ = InputBindingSet{};
  values_.recovered = result == LoadResult::Corrupt;
  if (values_.recovered) ESP_LOGW(kTag, "corrupt NVS record; restoring safe default profile");
  std::string error;
  const esp_err_t err = saveRecord(values_, error);
  values_.loaded = err == ESP_OK;
  if (err != ESP_OK) ESP_LOGE(kTag, "default record persist failed: %s", error.c_str());
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  return err;
}

InputBindingSet InputBindings::snapshot() const {
  ensureMutex();
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  const InputBindingSet out = values_;
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  return out;
}

bool InputBindings::update(const std::vector<InputBinding>& candidate, bool check_revision,
                           uint32_t expected_revision, const dashboard::Dashboard* dashboard,
                           std::string& error, bool& conflict) {
  conflict = false;
  error.clear();
  InputBindingErrors validation;
  if (!validateInputBindings(candidate, usableDashboard(dashboard), validation)) {
    error = validation.empty() ? "input binding validation failed"
                               : validation.front().field + ": " + validation.front().message;
    return false;
  }

  ensureMutex();
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  if (check_revision && values_.revision != expected_revision) {
    conflict = true;
    error = "input binding revision conflict";
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return false;
  }
  if (values_.bindings == candidate) {
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return true;
  }

  InputBindingSet next = values_;
  next.bindings = candidate;
  ++next.revision;
  if (next.revision == 0) next.revision = 1;
  next.loaded = false;
  next.recovered = false;
  if (saveRecord(next, error) != ESP_OK) {
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    return false;
  }
  next.loaded = true;
  values_ = std::move(next);
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  return true;
}

bool InputBindings::reset(bool check_revision, uint32_t expected_revision,
                          const dashboard::Dashboard* dashboard, std::string& error,
                          bool& conflict) {
  return update({}, check_revision, expected_revision, dashboard, error, conflict);
}

esp_err_t InputBindings::erasePersisted() {
  const esp_err_t nvs_err = ensureNvs();
  if (nvs_err != ESP_OK) return nvs_err;
  nvs_handle_t handle;
  esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
  if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
  if (err != ESP_OK) return err;
  err = nvs_erase_all(handle);
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  return err;
}

}  // namespace eego::admin
