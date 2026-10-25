// SPDX-License-Identifier: GPL-3.0-or-later
// Panel geometry shared by the renderer and touch hit-testing. Every function
// here is the exact code path the renderer uses to place what it draws, so a
// hit-test never has to guess title or row heights.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "dashboard_core/model.h"
#include "dashboard_ui/geometry.h"
#include "dashboard_ui/layout.h"
#include "font_manager.h"

namespace eego::dashboard::ui {

// Frame chrome of a (non-button) panel: the padded body, and whether a title
// row is drawn above it (title_row is empty otherwise).
struct PanelChrome {
  Rect body;
  Rect title_row;
  bool title = false;
};
PanelChrome panelChrome(FontManager& font, const PanelSpec& panel, const Rect& frame);

// Display cap of every alert list (panel lists and the device alert pages).
constexpr size_t kAlertListMax = 256;

// The one alert display order: active first, active ones by severity
// (critical first), then newest first. Strict weak ordering.
bool alertEventBefore(const Event& lhs, const Event& rhs);
// Stable-sorts `events` by alertEventBefore, then truncates to kAlertListMax.
// Sorting happens over all matches before truncation, so an active critical
// event is never cut off by a flood of older cleared ones.
void orderAlertEvents(std::vector<const Event*>& events);

// Events an alert panel shows, in display order (orderAlertEvents over every
// matching event). Draw and hit-test both go through this.
std::vector<const Event*> alertPanelEvents(const Dashboard& dashboard, const PanelSpec& panel);

// Row geometry of the alert list drawn inside `body` (see drawAlertPanel).
struct AlertListLayout {
  std::vector<const Event*> events;  // alertPanelEvents() order
  std::vector<Rect> rows;            // rows[i] displays events[i]; rows.size() <= events.size()
  int16_t row_height = 0;
  bool two_lines = false;  // title + message line per row
  bool more = false;       // a "+N more" line follows the rows
  Rect more_bounds;
};
AlertListLayout layoutAlertList(FontManager& font, const Dashboard& dashboard,
                                const PanelSpec& panel, const Rect& body);

// Hit-test of an alert panel at (x, y) (screen coordinates). Returns the index
// into alertPanelEvents() of the row drawn there, or -1 when (x, y) is not on a
// drawn event row (title row, "+N more" line, empty space, other panel kinds).
int alertRowAt(FontManager& font, const Dashboard& dashboard, const PanelLayout& panel,
               int16_t x, int16_t y);
// Same, with FontManager::instance() and the event itself (nullptr = no row).
const Event* alertEventAt(const Dashboard& dashboard, const PanelLayout& panel, int16_t x,
                          int16_t y);

}  // namespace eego::dashboard::ui
