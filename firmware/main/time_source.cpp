// SPDX-License-Identifier: GPL-3.0-or-later
// 统一时间源实现 —— 见 time_source.h。
#include "time_source.h"

#include <ctime>
#include <sys/time.h>

#include "pcf8563_rtc.h"

namespace panel {

namespace {

// RTC 句柄（无状态驱动对象；legacy I2C 端口级锁保证跨任务共享安全）
eego::Pcf8563Rtc& timeRtc() {
  static eego::Pcf8563Rtc inst(I2C_NUM_0);
  return inst;
}

}  // namespace

uint64_t TimeSource::now() {
  const uint64_t sys = static_cast<uint64_t>(time(nullptr));
  if (sys >= kPlausibleFloor) return sys;
  const eego::RtcTime t = timeRtc().readTime();
  if (t.valid) return static_cast<uint64_t>(timeRtc().toEpoch(t));
  return sys;  // 退化值（0 附近）；调用方以 valid() 判定，不得当作有效时间
}

bool TimeSource::valid() {
  if (static_cast<uint64_t>(time(nullptr)) >= kPlausibleFloor) return true;
  return timeRtc().readTime().valid;
}

bool TimeSource::syncSystemFromRtc() {
  if (static_cast<uint64_t>(time(nullptr)) >= kPlausibleFloor) return false;
  const eego::RtcTime t = timeRtc().readTime();
  if (!t.valid) return false;
  const time_t epoch = timeRtc().toEpoch(t);
  struct timeval tv;
  tv.tv_sec = epoch;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  return true;
}

bool TimeSource::writeRtc(uint64_t unix_now) {
  time_t t = static_cast<time_t>(unix_now);
  struct tm tm_buf;
  gmtime_r(&t, &tm_buf);
  eego::RtcTime rt;
  rt.year = static_cast<uint16_t>(tm_buf.tm_year + 1900);
  rt.month = static_cast<uint8_t>(tm_buf.tm_mon + 1);
  rt.day = static_cast<uint8_t>(tm_buf.tm_mday);
  rt.hour = static_cast<uint8_t>(tm_buf.tm_hour);
  rt.minute = static_cast<uint8_t>(tm_buf.tm_min);
  rt.second = static_cast<uint8_t>(tm_buf.tm_sec);
  rt.weekday = static_cast<uint8_t>(tm_buf.tm_wday);  // 0=周日（与 RtcTime 约定一致）
  rt.valid = true;
  return timeRtc().writeTime(rt) == ESP_OK;
}

}  // namespace panel
