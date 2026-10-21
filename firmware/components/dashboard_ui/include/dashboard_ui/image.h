// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "dashboard_core/model.h"
#include "dashboard_ui/geometry.h"
#include "dashboard_ui/image_probe.h"
#include "image_renderer.h"

namespace eego::dashboard::ui {

struct ImageBuffer {
  uint8_t* data = nullptr;
  size_t capacity = 0;
  size_t size = 0;
};

class ImageProvider {
 public:
  virtual ~ImageProvider() = default;
  virtual bool load(const Asset& asset, std::string_view resolved_uri,
                    ImageBuffer output) = 0;
};

class SdImageProvider final : public ImageProvider {
 public:
  bool load(const Asset& asset, std::string_view resolved_uri,
            ImageBuffer output) override;
};

bool inspectImage(const uint8_t* data, size_t size, uint32_t& width, uint32_t& height);

class ImagePipeline {
 public:
  ImagePipeline();
  ~ImagePipeline();
  ImagePipeline(const ImagePipeline&) = delete;
  ImagePipeline& operator=(const ImagePipeline&) = delete;

  void setProvider(ImageProvider* provider) { provider_ = provider; }
  void draw(PortraitCanvas& canvas, const Dashboard& dashboard, const PanelSpec& panel,
            const Rect& bounds, const VariableValues& variables);
  void invalidate(std::string_view asset_id = {});
  void clear();
  size_t cacheBytes() const;
  size_t cacheHits() const { return cache_hits_; }
  size_t cacheMisses() const { return cache_misses_; }

 private:
  struct Impl;
  Impl* impl_;
  ImageProvider* provider_ = nullptr;
  SdImageProvider sd_provider_;
  size_t cache_hits_ = 0;
  size_t cache_misses_ = 0;
};

}  // namespace eego::dashboard::ui
