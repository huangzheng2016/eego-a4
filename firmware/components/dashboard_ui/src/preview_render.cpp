// SPDX-License-Identifier: GPL-3.0-or-later
#include "dashboard_ui/preview_render.h"

#include "dashboard_ui/renderer.h"

namespace eego::dashboard::ui {

PreviewRenderResult renderPreviewPage(PortraitCanvas& canvas, const Dashboard& dashboard,
                                      const PreviewRenderOptions& options) {
  PreviewRenderResult out;
  const Page* page = nullptr;
  for (const Page& item : dashboard.pages) {
    if (!item.visible) continue;
    if (options.page_id.empty() ? (page == nullptr || item.id == dashboard.default_page_id)
                                : item.id == options.page_id) {
      page = &item;
      out.screen = out.screen_count;
    }
    ++out.screen_count;
  }
  if (page == nullptr) {
    out.bad_request = true;
    out.error = "no visible page with this id";
    return out;
  }
  out.page_id = page->id;
  VariableValues defaults;
  if (options.variables == nullptr) {
    std::string error;
    if (!defaultVariableValues(dashboard.variables, {}, defaults, error)) defaults.clear();
  }
  canvas.resetClip();
  canvas.clear();
  DashboardRenderer renderer;
  renderer.setImageProvider(options.images);
  RenderOptions render;
  render.page_id = page->id;
  render.variables = options.variables != nullptr ? *options.variables : defaults;
  render.force_full = true;
  render.layout_config = options.layout_config;
  const RenderResult result = renderer.render(canvas, dashboard, render);
  canvas.resetClip();
  if (result.page == nullptr || !result.layout.valid) {
    out.error = "page layout failed";
    return out;
  }
  out.ok = true;
  return out;
}

void packGrayLevels(const Uc8279cDisplay& display, uint8_t* rows, size_t stride, bool fast) {
  for (int16_t y = 0; y < static_cast<int16_t>(Uc8279cDisplay::HEIGHT); ++y) {
    uint8_t* row = rows + static_cast<size_t>(y) * stride;
    for (int16_t x = 0; x < static_cast<int16_t>(Uc8279cDisplay::WIDTH); x += 2) {
      uint8_t left = display.grayLevel(x, y);
      uint8_t right = display.grayLevel(static_cast<int16_t>(x + 1), y);
      if (fast) {
        left = left >= 2 ? 3 : 0;
        right = right >= 2 ? 3 : 0;
      }
      row[x / 2] = static_cast<uint8_t>((left << 4) | right);
    }
  }
}

}  // namespace eego::dashboard::ui
