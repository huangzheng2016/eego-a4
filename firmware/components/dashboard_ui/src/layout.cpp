// SPDX-License-Identifier: GPL-3.0-or-later
#include "dashboard_ui/layout.h"

#include <algorithm>

namespace eego::dashboard::ui {
namespace {

// Proportional edge mapping: boundary b of n cells over `length` pixels with
// `gap` gutters. A panel spanning [a, b) occupies [edge(a), edge(b) - gap), so
// adjacent panels are always separated by exactly one gutter and never overlap.
int32_t edge(uint64_t boundary, uint64_t cells, int32_t length, int32_t gap) {
  return static_cast<int32_t>(boundary * static_cast<uint64_t>(length + gap) / cells);
}

}  // namespace

PageLayout buildPageLayout(const Page& page, size_t page_index, const LayoutConfig& config) {
  PageLayout out;
  out.page_index = page_index;
  out.content = config.content;

  uint64_t last_screen = 0;
  for (const PanelSpec& panel : page.panels) {
    if (!panel.visible) continue;
    last_screen = std::max<uint64_t>(last_screen, gridScreenIndex(panel.grid));
  }
  out.page_count = static_cast<size_t>(last_screen + 1);

  if (page.grid_columns == 0 || config.content.empty() || config.gap_x < 0 ||
      config.gap_y < 0 || config.panel_inset < 0) {
    return out;
  }
  out.valid = true;
  if (page_index >= out.page_count) return out;

  const int32_t width = config.content.w;
  const int32_t height = config.content.h;
  for (size_t index = 0; index < page.panels.size(); ++index) {
    const PanelSpec& panel = page.panels[index];
    if (!panel.visible || panel.grid.w == 0 || panel.grid.h == 0) continue;
    if (gridScreenIndex(panel.grid) != page_index || panel.grid.x >= page.grid_columns) continue;

    const uint32_t column = panel.grid.x;
    const uint32_t column_span = std::min<uint32_t>(panel.grid.w, page.grid_columns - column);
    const uint32_t row = gridScreenRow(panel.grid);
    const uint32_t row_span = std::min<uint32_t>(panel.grid.h, kRowsPerPage - row);

    const int32_t left = edge(column, page.grid_columns, width, config.gap_x);
    const int32_t right = edge(static_cast<uint64_t>(column) + column_span, page.grid_columns,
                               width, config.gap_x) - config.gap_x;
    const int32_t top = edge(row, kRowsPerPage, height, config.gap_y);
    const int32_t bottom = edge(static_cast<uint64_t>(row) + row_span, kRowsPerPage, height,
                                config.gap_y) - config.gap_y;
    if (right <= left || bottom <= top) continue;

    PanelLayout item;
    item.panel = &panel;
    item.panel_index = index;
    item.bounds = clipTo({static_cast<int16_t>(config.content.x + left),
                          static_cast<int16_t>(config.content.y + top),
                          static_cast<int16_t>(right - left),
                          static_cast<int16_t>(bottom - top)},
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
