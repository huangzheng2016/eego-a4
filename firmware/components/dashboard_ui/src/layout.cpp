// SPDX-License-Identifier: GPL-3.0-or-later
#include "dashboard_ui/layout.h"

#include <algorithm>

namespace eego::dashboard::ui {
namespace {

uint32_t logicalColumnStart(uint32_t x, uint32_t grid_columns) {
  return std::min<uint64_t>(kLogicalColumns,
                            static_cast<uint64_t>(x) * kLogicalColumns / grid_columns);
}

uint32_t logicalColumnEnd(uint32_t x, uint32_t w, uint32_t grid_columns) {
  const uint64_t end = static_cast<uint64_t>(x) + w;
  const uint64_t logical = (end * kLogicalColumns + grid_columns - 1) / grid_columns;
  return static_cast<uint32_t>(std::min<uint64_t>(kLogicalColumns, logical));
}

}  // namespace

PageLayout buildPageLayout(const Page& page, size_t page_index, const LayoutConfig& config) {
  PageLayout out;
  out.page_index = page_index;
  out.content = config.content;

  uint64_t last_row = 0;
  for (const PanelSpec& panel : page.panels) {
    if (!panel.visible) continue;
    last_row = std::max(last_row, static_cast<uint64_t>(panel.grid.y) + panel.grid.h);
  }
  out.page_count = static_cast<size_t>(std::max<uint64_t>(
      1, (last_row + kRowsPerPage - 1) / kRowsPerPage));

  if (page.grid_columns == 0 || config.content.empty() || config.gap_x < 0 ||
      config.gap_y < 0 || config.panel_inset < 0) {
    return out;
  }
  const int32_t cell_w = (static_cast<int32_t>(config.content.w) -
                          (kLogicalColumns - 1) * config.gap_x) /
                         kLogicalColumns;
  const int32_t cell_h = (static_cast<int32_t>(config.content.h) -
                          (kRowsPerPage - 1) * config.gap_y) /
                         kRowsPerPage;
  if (cell_w <= 0 || cell_h <= 0 || cell_w > INT16_MAX || cell_h > INT16_MAX) return out;
  out.valid = true;
  if (page_index >= out.page_count) return out;

  const uint64_t page_row = static_cast<uint64_t>(page_index) * kRowsPerPage;
  for (size_t index = 0; index < page.panels.size(); ++index) {
    const PanelSpec& panel = page.panels[index];
    if (!panel.visible) continue;
    const uint64_t panel_end = static_cast<uint64_t>(panel.grid.y) + panel.grid.h;
    const uint64_t page_end = page_row + kRowsPerPage;
    const uint64_t row_start = std::max<uint64_t>(panel.grid.y, page_row);
    const uint64_t row_end = std::min(panel_end, page_end);
    if (row_start >= row_end || panel.grid.x >= page.grid_columns) continue;

    const uint32_t column = logicalColumnStart(panel.grid.x, page.grid_columns);
    const uint32_t column_end = logicalColumnEnd(
        panel.grid.x, std::min<uint32_t>(panel.grid.w, page.grid_columns - panel.grid.x),
        page.grid_columns);
    if (column >= column_end) continue;
    const uint32_t row = static_cast<uint32_t>(row_start - page_row);
    const uint32_t row_span = static_cast<uint32_t>(row_end - row_start);
    const uint32_t column_span = column_end - column;

    const int32_t x = config.content.x + column * (cell_w + config.gap_x);
    const int32_t y = config.content.y + row * (cell_h + config.gap_y);
    const int32_t w = column_span * cell_w + (column_span - 1) * config.gap_x;
    const int32_t h = row_span * cell_h + (row_span - 1) * config.gap_y;
    PanelLayout item;
    item.panel = &panel;
    item.panel_index = index;
    item.bounds = clipTo({static_cast<int16_t>(x), static_cast<int16_t>(y),
                          static_cast<int16_t>(w), static_cast<int16_t>(h)},
                         config.content);
    item.content = inset(item.bounds, config.panel_inset);
    item.column = column;
    item.row = row;
    item.column_span = column_span;
    item.row_span = row_span;
    if (!item.bounds.empty()) out.panels.push_back(item);
  }
  return out;
}

Rect actionBounds(const PanelLayout& panel, size_t action_count, size_t action_index,
                  int16_t gap) {
  if (action_count == 0 || action_index >= action_count || panel.content.empty() || gap < 0)
    return {};
  const size_t columns = action_count == 1 ? 1 : (action_count == 3 && panel.content.w >= 264 ? 3 : 2);
  const size_t rows = (action_count + columns - 1) / columns;
  const int32_t width = (static_cast<int32_t>(panel.content.w) -
                         static_cast<int32_t>(columns - 1) * gap) /
                        static_cast<int32_t>(columns);
  const int32_t height = (static_cast<int32_t>(panel.content.h) -
                          static_cast<int32_t>(rows - 1) * gap) /
                         static_cast<int32_t>(rows);
  if (width <= 0 || height <= 0) return {};
  const size_t column = action_index % columns;
  const size_t row = action_index / columns;
  return {static_cast<int16_t>(panel.content.x + column * (width + gap)),
          static_cast<int16_t>(panel.content.y + row * (height + gap)),
          static_cast<int16_t>(width), static_cast<int16_t>(height)};
}

bool hitTest(const PageLayout& layout, int16_t x, int16_t y, HitTarget& target,
             int16_t button_gap) {
  for (auto panel = layout.panels.rbegin(); panel != layout.panels.rend(); ++panel) {
    if (panel->panel == nullptr || panel->panel->kind != PanelKind::Button ||
        !panel->bounds.contains(x, y)) {
      continue;
    }
    for (size_t index = panel->panel->actions.size(); index-- > 0;) {
      const Rect bounds = actionBounds(*panel, panel->panel->actions.size(), index, button_gap);
      if (!bounds.contains(x, y)) continue;
      target.panel = panel->panel;
      target.action = &panel->panel->actions[index];
      target.action_index = index;
      target.bounds = bounds;
      return true;
    }
    return false;
  }
  target = {};
  return false;
}

bool findPanelBounds(const PageLayout& layout, std::string_view panel_id, Rect& bounds) {
  for (auto panel = layout.panels.rbegin(); panel != layout.panels.rend(); ++panel) {
    if (panel->panel != nullptr && panel->panel->id == panel_id) {
      bounds = panel->bounds;
      return true;
    }
  }
  bounds = {};
  return false;
}

}  // namespace eego::dashboard::ui
