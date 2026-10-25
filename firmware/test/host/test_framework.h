// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal self-contained host test framework (no external downloads).
//
//   TEST(suite, name) { CHECK(cond); CHECK_EQ(a, b); REQUIRE(cond); }
//
// The runner (test_main.cpp) accepts `--suite <name>` and `--list`.
#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <type_traits>
#include <sstream>
#include <string>
#include <vector>

namespace hosttest {

struct TestCase {
  const char* suite;
  const char* name;
  void (*fn)();
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

struct Registrar {
  Registrar(const char* suite, const char* name, void (*fn)()) {
    registry().push_back({suite, name, fn});
  }
};

struct Failure {};  // thrown by REQUIRE to abort the current test

inline int& currentFailures() {
  static int failures = 0;
  return failures;
}

inline void reportFailure(const char* file, int line, const std::string& message) {
  ++currentFailures();
  std::fprintf(stderr, "    %s:%d: FAILED: %s\n", file, line, message.c_str());
}

template <typename T>
std::string show(const T& value) {
  std::ostringstream out;
  if constexpr (std::is_enum_v<T>) {
    out << static_cast<long long>(value);
  } else {
    out << value;
  }
  return out.str();
}

}  // namespace hosttest

#define HT_CONCAT2(a, b) a##b
#define HT_CONCAT(a, b) HT_CONCAT2(a, b)

#define TEST(suite, name)                                                         \
  static void HT_CONCAT(test_##suite##_, name)();                                 \
  static ::hosttest::Registrar HT_CONCAT(reg_##suite##_, name)(#suite, #name,     \
                                                               &HT_CONCAT(test_##suite##_, name)); \
  static void HT_CONCAT(test_##suite##_, name)()

#define CHECK(cond)                                                               \
  do {                                                                            \
    if (!(cond)) ::hosttest::reportFailure(__FILE__, __LINE__, #cond);            \
  } while (0)

#define CHECK_MSG(cond, msg)                                                      \
  do {                                                                            \
    if (!(cond)) {                                                                \
      std::ostringstream ht_os_;                                                  \
      ht_os_ << #cond << " -- " << msg;                                           \
      ::hosttest::reportFailure(__FILE__, __LINE__, ht_os_.str());                \
    }                                                                             \
  } while (0)

#define REQUIRE(cond)                                                             \
  do {                                                                            \
    if (!(cond)) {                                                                \
      ::hosttest::reportFailure(__FILE__, __LINE__, "REQUIRE " #cond);            \
      throw ::hosttest::Failure{};                                                \
    }                                                                             \
  } while (0)

#define REQUIRE_MSG(cond, msg)                                                    \
  do {                                                                            \
    if (!(cond)) {                                                                \
      std::ostringstream ht_os_;                                                  \
      ht_os_ << "REQUIRE " << #cond << " -- " << msg;                             \
      ::hosttest::reportFailure(__FILE__, __LINE__, ht_os_.str());                \
      throw ::hosttest::Failure{};                                                \
    }                                                                             \
  } while (0)

#define CHECK_EQ(a, b)                                                            \
  do {                                                                            \
    const auto& ht_a_ = (a);                                                      \
    const auto& ht_b_ = (b);                                                      \
    if (!(ht_a_ == ht_b_))                                                        \
      ::hosttest::reportFailure(__FILE__, __LINE__,                               \
                                std::string(#a " == " #b " (") +                  \
                                    ::hosttest::show(ht_a_) + " vs " +            \
                                    ::hosttest::show(ht_b_) + ")");               \
  } while (0)

#define CHECK_NEAR(a, b, eps)                                                     \
  do {                                                                            \
    const double ht_a_ = (a);                                                     \
    const double ht_b_ = (b);                                                     \
    if (!(std::fabs(ht_a_ - ht_b_) <= (eps)))                                     \
      ::hosttest::reportFailure(__FILE__, __LINE__,                               \
                                std::string(#a " ~= " #b " (") +                  \
                                    ::hosttest::show(ht_a_) + " vs " +            \
                                    ::hosttest::show(ht_b_) + ")");               \
  } while (0)
