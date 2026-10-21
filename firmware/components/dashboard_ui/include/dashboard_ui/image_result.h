// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "data_pipeline/types.h"

namespace eego::dashboard::ui {

inline bool dashboardAssetMatchesSource(const Asset& asset, const Source& source,
                                        const VariableValues& variables) {
  if (asset.id == source.id) return true;
  std::string source_url;
  std::string asset_uri;
  std::string error;
  return interpolateVariables(source.url, variables, source_url, error, 4096) &&
         interpolateVariables(asset.uri, variables, asset_uri, error, 4096) &&
         source_url == asset_uri;
}

inline bool dashboardSourceMatchesUri(const Source& source, std::string_view resolved_uri,
                                      const VariableValues& variables) {
  std::string expanded;
  std::string error;
  return interpolateVariables(source.url, variables, expanded, error, 4096) &&
         expanded == resolved_uri;
}

inline const Source* dashboardImageSourceForUri(const Asset& asset,
                                                std::string_view resolved_uri,
                                                const VariableValues& variables,
                                                const std::vector<Source>& sources) {
  for (const Source& source : sources) {
    if (dashboardSourceMatchesUri(source, resolved_uri, variables)) return &source;
  }
  std::string base_uri;
  std::string error;
  if (interpolateVariables(asset.uri, variables, base_uri, error, 4096) &&
      base_uri == resolved_uri) {
    for (const Source& source : sources) {
      if (source.id == asset.id) return &source;
    }
  }
  return nullptr;
}

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
