// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

//
//

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace eego::admin {
enum class LogLevel : uint8_t {
  Error = 0,
  Warn = 1,
  Info = 2,
  Debug = 3,
};

const char* logLevelName(LogLevel lv);
bool logLevelFromString(const std::string& s, LogLevel& out);

struct LogEntry {
  uint64_t ts = 0;
  bool unsynced = false;
  LogLevel level = LogLevel::Info;
  uint32_t seq = 0;
  char msg[176];
};

class AdminLog {
 public:
  static AdminLog& instance();

  void init(LogLevel min_level = LogLevel::Info);

  void setMinLevel(LogLevel lv);

  void log(LogLevel lv, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

  std::vector<LogEntry> recent(size_t max_entries) const;

  static std::string maskSecret(const std::string& s);

 private:
  AdminLog() = default;

  mutable void* mutex_ = nullptr;
  LogLevel min_level_ = LogLevel::Info;
  std::vector<LogEntry> ring_;
  size_t head_ = 0;
  bool full_ = false;
  uint32_t next_seq_ = 1;
};

#define ADMIN_LOG_ERROR(...) ::eego::admin::AdminLog::instance().log(::eego::admin::LogLevel::Error, __VA_ARGS__)
#define ADMIN_LOG_WARN(...) ::eego::admin::AdminLog::instance().log(::eego::admin::LogLevel::Warn, __VA_ARGS__)
#define ADMIN_LOG_INFO(...) ::eego::admin::AdminLog::instance().log(::eego::admin::LogLevel::Info, __VA_ARGS__)
#define ADMIN_LOG_DEBUG(...) ::eego::admin::AdminLog::instance().log(::eego::admin::LogLevel::Debug, __VA_ARGS__)

}  // namespace eego::admin
