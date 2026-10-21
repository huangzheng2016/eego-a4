// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "data_pipeline/types.h"

namespace eego::dashboard::ui {

inline bool copySourceImage(const pipeline::SourceResult& result, uint8_t* output,
                            size_t capacity, size_t& output_size) {
  output_size = 0;
  if (!result.has_image || result.image.bytes.empty() || output == nullptr ||
      result.image.bytes.size() > capacity) {
    return false;
  }
  std::memcpy(output, result.image.bytes.data(), result.image.bytes.size());
  output_size = result.image.bytes.size();
  return true;
}

}  // namespace eego::dashboard::ui
