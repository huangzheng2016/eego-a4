// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// 调试输出：把 ESP_LOG（E/W/I）同时留一份在 PSRAM 环形缓冲里，网页「系统 → 设备日志」
// 可直接查看；并在上次异常重启时从 Flash coredump 分区读出崩溃摘要。
// 串口照常输出（idf.py monitor 可看到完整日志和解码后的回溯）。

#include <cstddef>
#include <string>

#include "esp_err.h"

namespace eego::admin::devlog {

// 在调度器启动后调用一次（幂等）。
void install();

// 最近的日志文本（整行，最多 max_bytes 字节）。
std::string tail(size_t max_bytes);

// {"reset_reason":"panic","crashed":true,"task":"...","pc":"0x...","backtrace":["0x..."],
//  "cause":n,"vaddr":"0x...","elf_sha256":"..."}；无 coredump 时 crashed=false。
std::string crashJson();

// 擦除 coredump（网页「清除」）。
esp_err_t clearCrash();

// 开机时把上次的崩溃摘要写进日志（串口与环形缓冲都能看到）。
void logBootReport();

}  // namespace eego::admin::devlog
