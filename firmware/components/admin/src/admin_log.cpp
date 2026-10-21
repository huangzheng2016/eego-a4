// SPDX-License-Identifier: GPL-3.0-or-later
// 管理日志实现 —— 见 include/admin/admin_log.h。
#include "admin/admin_log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace eego::admin {

namespace {
constexpr size_t kRingCapacity = 128;
constexpr int64_t kWallClockMinUnix = 1700000000;  // 2023-11-14，低于此视为未同步
const char* TAG = "admin.log";

// 串口镜像的级别映射（secrets 边界由调用点保证，这里不再过滤）。
esp_log_level_t toEsp(LogLevel lv) {
  switch (lv) {
    case LogLevel::Error: return ESP_LOG_ERROR;
    case LogLevel::Warn: return ESP_LOG_WARN;
    case LogLevel::Info: return ESP_LOG_INFO;
    case LogLevel::Debug: return ESP_LOG_DEBUG;
  }
  return ESP_LOG_INFO;
}
}  // namespace

const char* logLevelName(LogLevel lv) {
  switch (lv) {
    case LogLevel::Error: return "ERROR";
    case LogLevel::Warn: return "WARN";
    case LogLevel::Info: return "INFO";
    case LogLevel::Debug: return "DEBUG";
  }
  return "INFO";
}

bool logLevelFromString(const std::string& s, LogLevel& out) {
  if (s == "ERROR") { out = LogLevel::Error; return true; }
  if (s == "WARN") { out = LogLevel::Warn; return true; }
  if (s == "INFO") { out = LogLevel::Info; return true; }
  if (s == "DEBUG") { out = LogLevel::Debug; return true; }
  return false;
}

AdminLog& AdminLog::instance() {
  static AdminLog inst;
  return inst;
}

void AdminLog::init(LogLevel min_level) {
  if (mutex_ == nullptr) {
    mutex_ = xSemaphoreCreateMutex();
    ring_.reserve(kRingCapacity);
  }
  min_level_ = min_level;
}

void AdminLog::setMinLevel(LogLevel lv) {
  if (mutex_ == nullptr) init(lv);
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  min_level_ = lv;
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
}

void AdminLog::log(LogLevel lv, const char* fmt, ...) {
  if (mutex_ == nullptr) init(min_level_);
  if (lv > min_level_) return;

  LogEntry e;
  const int64_t now_unix = static_cast<int64_t>(time(nullptr));
  if (now_unix >= kWallClockMinUnix) {
    e.ts = static_cast<uint64_t>(now_unix);
    e.unsynced = false;
  } else {
    e.ts = static_cast<uint64_t>(esp_timer_get_time() / 1000000);
    e.unsynced = true;
  }
  e.level = lv;

  va_list ap;
  va_start(ap, fmt);
  vsnprintf(e.msg, sizeof(e.msg), fmt, ap);
  va_end(ap);
  e.msg[sizeof(e.msg) - 1] = '\0';

  esp_log_write(toEsp(lv), TAG, "%s", e.msg);

  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  e.seq = next_seq_++;
  if (ring_.size() < kRingCapacity) {
    ring_.push_back(e);
  } else {
    ring_[head_] = e;
    head_ = (head_ + 1) % kRingCapacity;
    full_ = true;
  }
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
}

std::vector<LogEntry> AdminLog::recent(size_t max_entries) const {
  std::vector<LogEntry> out;
  if (mutex_ == nullptr) return out;
  xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
  const size_t n = ring_.size();
  const size_t count = (max_entries < n) ? max_entries : n;
  out.reserve(count);
  for (size_t i = 0; i < count; i++) {
    // 最旧在前：未满时 0..n-1；满后从 head_ 起绕环
    size_t idx = full_ ? (head_ + n - count + i) % n : (n - count + i);
    out.push_back(ring_[idx]);
  }
  xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
  return out;
}

std::string AdminLog::maskSecret(const std::string& s) {
  if (s.size() >= 8) return s.substr(0, 4) + "***";
  return "***";
}

}  // namespace eego::admin
