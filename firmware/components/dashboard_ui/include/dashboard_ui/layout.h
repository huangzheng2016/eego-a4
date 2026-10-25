// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "dashboard_core/model.h"
#include "dashboard_ui/geometry.h"

namespace eego::dashboard::ui {

// One device screen ("subpage") holds kRowsPerPage grid rows. Columns are
// mapped proportionally from page.grid_columns; see kGridRowsPerScreen in
// dashboard_core/model.h for the full rule shared with validation.
inline constexpr uint32_t kRowsPerPage = kGridRowsPerScreen;

struct LayoutConfig {
  Rect content = kDashboardContent;
  int16_t gap_x = 8;
  int16_t gap_y = 8;
  int16_t panel_inset = 6;
  int16_t button_gap = 4;
};

struct PanelLayout {
  const PanelSpec* panel = nullptr;
  size_t panel_index = 0;
  Rect bounds;
  Rect content;
  // Grid coordinates: column/column_span in page.grid_columns units, row and
  // row_span in grid rows relative to the top of the current screen.
  uint32_t column = 0;
  uint32_t row = 0;
  uint32_t column_span = 0;
  uint32_t row_span = 0;
};

struct PageLayout {
  std::vector<PanelLayout> panels;
  size_t page_index = 0;
  size_t page_count = 1;
  Rect content = kDashboardContent;
  bool valid = false;
};

struct HitTarget {
  const PanelSpec* panel = nullptr;
  const Action* action = nullptr;
  size_t action_index = 0;
  Rect bounds;
};

// Screen index of a panel on its page (same rule as buildPageLayout).
inline size_t panelScreenIndex(const PanelSpec& panel) { return gridScreenIndex(panel.grid); }

PageLayout buildPageLayout(const Page& page, size_t page_index,
                           const LayoutConfig& config = {});
Rect actionBounds(const PanelLayout& panel, size_t action_count, size_t action_index,
                  int16_t gap = 4);
bool hitTest(const PageLayout& layout, int16_t x, int16_t y, HitTarget& target,
             int16_t button_gap = 4);
bool findPanelBounds(const PageLayout& layout, std::string_view panel_id, Rect& bounds);

}  // namespace eego::dashboard::ui
