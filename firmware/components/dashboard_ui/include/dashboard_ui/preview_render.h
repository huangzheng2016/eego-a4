// SPDX-License-Identifier: GPL-3.0-or-later
// Offscreen preview render of one page/screen of a dashboard with the real
// renderer, plus 4-bpp packing of the result (admin render-preview endpoint).
// The canvas must wrap an offscreen Uc8279cDisplay (beginOffscreen()), never
// the live display.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "dashboard_core/model.h"
#include "dashboard_ui/image.h"
#include "dashboard_ui/layout.h"
#include "portrait_canvas.h"
#include "uc8279c_display.h"

namespace eego::dashboard::ui {

struct PreviewRenderOptions {
  std::string page_id;          // empty = default page / first visible page
  size_t screen = 0;
  LayoutConfig layout_config;   // same content area as the device runtime
  const VariableValues* variables = nullptr;  // null = the dashboard's defaults
  ImageProvider* images = nullptr;            // null = image panels show their fallback
};

struct PreviewRenderResult {
  bool ok = false;
  bool bad_request = false;  // unknown page / screen out of range (vs. internal failure)
  std::string error;
  std::string page_id;
  size_t screen = 0;
  size_t screen_count = 0;
};

// Clears the canvas and renders the page/screen with a fresh DashboardRenderer.
PreviewRenderResult renderPreviewPage(PortraitCanvas& canvas, const Dashboard& dashboard,
                                      const PreviewRenderOptions& options);

// Packs the display's pixels as 4-bpp rows (high nibble = even x), value = gray
// level 0..3 (Uc8279cDisplay::grayLevel). `fast` thresholds like a fast
// refresh: levels 0-1 -> 0 (black), 2-3 -> 3 (white).
void packGrayLevels(const Uc8279cDisplay& display, uint8_t* rows, size_t stride, bool fast);

}  // namespace eego::dashboard::ui
