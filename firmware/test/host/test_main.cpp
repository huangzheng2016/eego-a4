// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstring>
#include <exception>

#include "test_framework.h"

int main(int argc, char** argv) {
  const char* suite = nullptr;
  bool list = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--suite") == 0 && i + 1 < argc) {
      suite = argv[++i];
    } else if (std::strcmp(argv[i], "--list") == 0) {
      list = true;
    } else {
      std::fprintf(stderr, "usage: %s [--suite NAME] [--list]\n", argv[0]);
      return 2;
    }
  }
  int run = 0;
  int failed = 0;
  for (const auto& test : hosttest::registry()) {
    if (suite != nullptr && std::strcmp(suite, test.suite) != 0) continue;
    if (list) {
      std::printf("%s.%s\n", test.suite, test.name);
      continue;
    }
    ++run;
    hosttest::currentFailures() = 0;
    try {
      test.fn();
    } catch (const hosttest::Failure&) {
    } catch (const std::exception& e) {
      hosttest::reportFailure(__FILE__, __LINE__, std::string("exception: ") + e.what());
    } catch (...) {
      hosttest::reportFailure(__FILE__, __LINE__, "unknown exception");
    }
    const bool ok = hosttest::currentFailures() == 0;
    if (!ok) ++failed;
    std::printf("[%s] %s.%s\n", ok ? " OK " : "FAIL", test.suite, test.name);
  }
  if (list) return 0;
  if (run == 0) {
    std::fprintf(stderr, "no tests matched\n");
    return 1;
  }
  std::printf("%d tests, %d passed, %d failed\n", run, run - failed, failed);
  return failed == 0 ? 0 : 1;
}
