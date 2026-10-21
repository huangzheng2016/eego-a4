#include "alert_core/time_util.h"

#include <cstdio>
#include <ctime>

namespace alert_core {
namespace {

// Howard Hinnant's civil-calendar algorithms (public domain, chrono.date).
constexpr int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? static_cast<unsigned>(-3) : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civilFromDays(int64_t z, int64_t& y, unsigned& m, unsigned& d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t yy = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp + (mp < 10 ? 3 : static_cast<unsigned>(-9));
  y = yy + (m <= 2);
}

bool isLeapYear(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

unsigned daysInMonth(int64_t y, unsigned m) {
  static const unsigned kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m == 2 && isLeapYear(y)) return 29;
  return (m >= 1 && m <= 12) ? kDays[m - 1] : 0;
}

bool readDigits(const std::string& s, size_t pos, size_t count, int& out) {
  if (pos + count > s.size()) return false;
  int v = 0;
  for (size_t i = 0; i < count; ++i) {
    char c = s[pos + i];
    if (c < '0' || c > '9') return false;
    v = v * 10 + (c - '0');
  }
  out = v;
  return true;
}

bool expectChar(const std::string& s, size_t pos, char expected) {
  return pos < s.size() && s[pos] == expected;
}

}  // namespace

std::string formatUtcIso8601(uint64_t unix_seconds) {
  int64_t days = static_cast<int64_t>(unix_seconds / 86400);
  uint64_t rem = unix_seconds % 86400;
  int64_t y;
  unsigned m, d;
  civilFromDays(days, y, m, d);
  unsigned hh = static_cast<unsigned>(rem / 3600);
  unsigned mm = static_cast<unsigned>((rem % 3600) / 60);
  unsigned ss = static_cast<unsigned>(rem % 60);
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%04lld-%02u-%02uT%02u:%02u:%02uZ", static_cast<long long>(y),
                m, d, hh, mm, ss);
  return buf;
}

bool parseUtcIso8601(const std::string& s, uint64_t& unix_seconds_out) {
  // YYYY-MM-DDTHH:MM:SS with optional .fff and Z / +HH:MM / -HH:MM.
  int year, month, day, hour, minute, second;
  if (!readDigits(s, 0, 4, year) || !expectChar(s, 4, '-') || !readDigits(s, 5, 2, month) ||
      !expectChar(s, 7, '-') || !readDigits(s, 8, 2, day) ||
      (s.size() > 10 && (s[10] != 'T' && s[10] != 't')) ||
      !readDigits(s, 11, 2, hour) || !expectChar(s, 13, ':') || !readDigits(s, 14, 2, minute) ||
      !expectChar(s, 16, ':') || !readDigits(s, 17, 2, second)) {
    return false;
  }
  if (s.size() < 19) return false;
  if (month < 1 || month > 12) return false;
  if (day < 1 || static_cast<unsigned>(day) > daysInMonth(year, month)) return false;
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59)
    return false;

  size_t pos = 19;
  if (pos < s.size() && s[pos] == '.') {
    ++pos;
    size_t fracStart = pos;
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') ++pos;
    if (pos == fracStart) return false;  // "." with no digits
  }

  int64_t offset_seconds = 0;
  if (pos < s.size()) {
    char zone = s[pos];
    if (zone == 'Z' || zone == 'z') {
      ++pos;
    } else if (zone == '+' || zone == '-') {
      int oh, om;
      if (!readDigits(s, pos + 1, 2, oh) || !expectChar(s, pos + 3, ':') ||
          !readDigits(s, pos + 4, 2, om)) {
        return false;
      }
      if (oh > 23 || om > 59) return false;
      offset_seconds = static_cast<int64_t>(oh) * 3600 + om * 60;
      if (zone == '-') offset_seconds = -offset_seconds;
      pos += 6;
    } else {
      return false;
    }
  }
  if (pos != s.size()) return false;

  int64_t days = daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
  int64_t total = days * 86400 + static_cast<int64_t>(hour) * 3600 +
                  static_cast<int64_t>(minute) * 60 + second - offset_seconds;
  if (total < 0) return false;
  unix_seconds_out = static_cast<uint64_t>(total);
  return true;
}

}  // namespace alert_core
