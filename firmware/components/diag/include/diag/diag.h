// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

//
//

#include <cstdint>
#include <functional>

namespace eego::diag {
struct Handlers {
  std::function<void()> status;
  std::function<void()> battery;
  std::function<void()> rtc;                     // RTC wall-clock
  std::function<void()> sd;
  std::function<void()> net;
  std::function<void()> alerts;
  std::function<void(const char* json)> inject;
  std::function<void()> ack_all;
  std::function<void()> smoke;
  std::function<void()> version;
  std::function<void(const char* arg)> lang;
  std::function<void(const char* arg)> page;
  std::function<void(const char* arg)> dashboard;
  std::function<void(const char* arg)> touch;      // touch [trace]|cal|marks on|off|show|reset
  std::function<void(const char* arg)> wifi;
};

bool start(const Handlers& handlers);

void service();

bool injectLine(const char* line);

}  // namespace eego::diag
