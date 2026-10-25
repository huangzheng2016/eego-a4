// SPDX-License-Identifier: GPL-3.0-or-later
// Host render-harness stub: no SD card (images come from an ImageProvider).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
namespace eego {
class SdCard {
 public:
  static SdCard& instance() {
    static SdCard card;
    return card;
  }
  bool ready() const { return false; }
  FILE* open(const char*, const char*) { return nullptr; }
  size_t fileSize(const char*) { return 0; }
  size_t readFile(const char*, uint8_t*, size_t) { return 0; }
};
}  // namespace eego
