// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// 管理日志 —— RAM 环形缓冲 + 串口镜像（M13 F7）。
//
// 边界（强制）：
//   * 永不入日志：WiFi 密码、管理密码任何形态、完整 token/ingest key
//     （仅前 4 位掩码，用 maskSecret）、请求体原文、config.json 全文；
//   * 允许入日志：SSID、字段名、校验失败原因、来源 IP、状态码、耗时、
//     重连/窗口/恢复出厂等安全事件；
//   * 写入方负责不传 secrets（本模块只提供统一入口与掩码工具，
//     调用点见 admin_httpd.cpp / provision_portal.cpp 的事件记录）。
//
// 时间戳：time(nullptr) 有效（>= 2023-11-14）用 wall clock；否则用开机
// 毫秒并标 unsynced=1（M13 F7：RTC 有效用 RTC，否则开机毫秒并标 unsynced）。
// 本组件不触碰 I2C/RTC（避免与 input_task 并发访问同一总线）。

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
// 解析 "ERROR"|"WARN"|"INFO"|"DEBUG"（大小写不敏感）；失败返回 false。
bool logLevelFromString(const std::string& s, LogLevel& out);

struct LogEntry {
  uint64_t ts = 0;          // unix 秒（unsynced 时为开机秒）
  bool unsynced = false;    // true = 时间未同步（ts 为开机秒）
  LogLevel level = LogLevel::Info;
  uint32_t seq = 0;         // 单调递增序号（读取游标）
  char msg[176];            // 已截断到安全长度
};

class AdminLog {
 public:
  static AdminLog& instance();

  // 初始化（幂等）。min_level 以下级别直接丢弃（默认 INFO）。
  void init(LogLevel min_level = LogLevel::Info);

  void setMinLevel(LogLevel lv);

  // 线程安全；fmt 为 printf 格式，输出自动截断到 175 字符。
  // 成员函数含隐式 this：fmt 为第 3 参、varargs 从第 4 参起。
  void log(LogLevel lv, const char* fmt, ...) __attribute__((format(printf, 3, 4)));

  // 最近 N 条（最新在后）；最多返回 max_entries 条。
  std::vector<LogEntry> recent(size_t max_entries) const;

  // 掩码工具：长度 >=8 时保留前 4 位 + "***"，否则 "***"（M13：仅前 4 位可示）。
  static std::string maskSecret(const std::string& s);

 private:
  AdminLog() = default;

  mutable void* mutex_ = nullptr;  // SemaphoreHandle_t（构造延迟创建，见 .cpp）
  LogLevel min_level_ = LogLevel::Info;
  std::vector<LogEntry> ring_;
  size_t head_ = 0;  // ring_ 未满时等于元素个数；满后为最旧元素下标
  bool full_ = false;
  uint32_t next_seq_ = 1;
};

// 便捷宏：AdminLog::instance().log(...)
#define ADMIN_LOG_ERROR(...) ::eego::admin::AdminLog::instance().log(::eego::admin::LogLevel::Error, __VA_ARGS__)
#define ADMIN_LOG_WARN(...) ::eego::admin::AdminLog::instance().log(::eego::admin::LogLevel::Warn, __VA_ARGS__)
#define ADMIN_LOG_INFO(...) ::eego::admin::AdminLog::instance().log(::eego::admin::LogLevel::Info, __VA_ARGS__)
#define ADMIN_LOG_DEBUG(...) ::eego::admin::AdminLog::instance().log(::eego::admin::LogLevel::Debug, __VA_ARGS__)

}  // namespace eego::admin
