// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "dashboard_core/model.h"
#include "dashboard_ui/image.h"
#include "dashboard_ui/layout.h"
#include "portrait_canvas.h"

namespace eego::dashboard::ui {

struct RenderOptions {
  std::string page_id;
  VariableValues variables;
  bool force_full = false;
  // Content area / gutters used to place panels; must match the layout the
  // caller hit-tests against (e.g. excluding a reserved status line).
  LayoutConfig layout_config;
};

struct RenderResult {
  const Page* page = nullptr;
  PageLayout layout;
  std::vector<Rect> dirty_rects;
  RefreshMode refresh_mode = RefreshMode::Fast;
  bool refresh_needed = false;
  bool full_redraw = false;
  size_t dirty_panels = 0;
  size_t drawn_panels = 0;
  size_t image_cache_bytes = 0;
};

class DashboardRenderer {
 public:
  DashboardRenderer();
  ~DashboardRenderer();
  DashboardRenderer(const DashboardRenderer&) = delete;
  DashboardRenderer& operator=(const DashboardRenderer&) = delete;

  RenderResult render(PortraitCanvas& canvas, const Dashboard& dashboard,
                      const RenderOptions& options = {});
  void setImageProvider(ImageProvider* provider) { images_.setProvider(provider); }
  void invalidatePanel(std::string_view panel_id);
  void invalidateImage(std::string_view asset_id = {});
  void reset();
  size_t imageCacheBytes() const { return images_.cacheBytes(); }

 private:
  struct Impl;
  Impl* impl_;
  ImagePipeline images_;
  uint64_t image_generation_ = 0;
  std::vector<std::pair<std::string, uint64_t>> asset_generations_;
};

}  // namespace eego::dashboard::ui
