// SPDX-License-Identifier: GPL-3.0-or-later
// Alert-panel hit-testing vs. what the real renderer draws: renders alert
// panels with the dashboard_ui renderer (fallback font) into the host
// framebuffer, finds the drawn rows by their severity markers, and checks that
// ui::alertRowAt / alertEventAt map every drawn row back to the same event.
#include <memory>
#include <string>
#include <vector>

#include "dashboard_ui/layout.h"
#include "dashboard_ui/panel_hit.h"
#include "dashboard_ui/renderer.h"
#include "font_manager.h"
#include "host_font.h"
#include "portrait_canvas.h"
#include "test_framework.h"
#include "uc8279c_display.h"

namespace {

using namespace eego::dashboard;

Dashboard alertDashboard(uint32_t grid_h, size_t events, const std::string& title = "Alerts",
                         bool messages = false) {
  Dashboard dash;
  dash.id = "hit";
  Page page;
  page.id = "main";
  page.grid_columns = 24;
  PanelSpec panel;
  panel.id = "alerts";
  panel.kind = PanelKind::Alert;
  panel.title = title;
  panel.grid = {0, 0, 24, grid_h};
  page.panels.push_back(panel);
  dash.pages.push_back(page);
  dash.default_page_id = "main";
  for (size_t i = 0; i < events; ++i) {
    Event event;
    event.id = "e" + std::to_string(i);
    event.title = "Event " + std::to_string(i);
    event.timestamp_ms = 1700000000000LL + static_cast<int64_t>(i) * 60000;
    // Mixed order/severity so the display order differs from insertion order.
    event.severity = i % 3 == 0 ? Severity::Warning : Severity::Critical;
    event.state = i == 1 ? EventState::Cleared : EventState::Active;
    if (messages) event.message = "detail " + std::to_string(i);
    dash.events.push_back(event);
  }
  return dash;
}

struct Rendered {
  std::unique_ptr<eego::Uc8279cDisplay> display = std::make_unique<eego::Uc8279cDisplay>();
  ui::PageLayout layout;
};

Rendered render(const Dashboard& dash) {
  eego::render_host::useFallbackFont();
  Rendered out;
  eego::PortraitCanvas canvas(*out.display);
  canvas.clear();
  ui::DashboardRenderer renderer;
  ui::RenderOptions options;
  options.page_id = "main";
  options.force_full = true;
  const ui::RenderResult result = renderer.render(canvas, dash, options);
  out.layout = result.layout;
  return out;
}

// Vertical bands where the severity-marker column holds ink. Markers (filled
// square, triangle, circle) are several px tall; the dotted row separators are
// 1 px lines and are dropped.
std::vector<std::pair<int, int>> markerBands(const eego::Uc8279cDisplay& display,
                                             const ui::Rect& body, int marker) {
  std::vector<std::pair<int, int>> bands;
  int start = -1;
  for (int y = body.y; y <= body.bottom(); ++y) {
    bool ink = false;
    for (int x = body.x; x < body.x + marker && y < body.bottom(); ++x)
      ink = ink || display.grayAt(static_cast<int16_t>(x), static_cast<int16_t>(y)) < 8;
    if (ink && start < 0) start = y;
    if (!ink && start >= 0) {
      if (y - start >= 3) bands.emplace_back(start, y);
      start = -1;
    }
  }
  return bands;
}

void checkRowsMatchDrawing(const Dashboard& dash, size_t expect_min_rows) {
  Rendered rendered = render(dash);
  REQUIRE(rendered.layout.panels.size() == 1);
  const ui::PanelLayout& item = rendered.layout.panels.front();
  eego::FontManager& font = eego::FontManager::instance();
  const ui::PanelChrome chrome = ui::panelChrome(font, *item.panel, item.bounds);
  const ui::AlertListLayout list = ui::layoutAlertList(font, dash, *item.panel, chrome.body);
  const std::vector<const Event*> order = ui::alertPanelEvents(dash, *item.panel);
  REQUIRE(list.rows.size() >= expect_min_rows);
  REQUIRE(order.size() == dash.events.size());

  const int caption = font.uiFontHeight();
  const int marker = std::max(6, caption / 2);
  const auto bands = markerBands(*rendered.display, chrome.body, marker);
  CHECK_EQ(bands.size(), list.rows.size());
  for (size_t i = 0; i < bands.size() && i < list.rows.size(); ++i) {
    const int16_t cy = static_cast<int16_t>((bands[i].first + bands[i].second) / 2);
    // Marker centre, the text column and the right edge of the drawn row.
    for (const int16_t x : {static_cast<int16_t>(chrome.body.x + marker / 2),
                            static_cast<int16_t>(chrome.body.x + chrome.body.w / 2),
                            static_cast<int16_t>(chrome.body.right() - 1)}) {
      CHECK_EQ(ui::alertRowAt(font, dash, item, x, cy), static_cast<int>(i));
      CHECK(ui::alertEventAt(dash, item, x, cy) == order[i]);
    }
    // The whole drawn row maps to the same event (top and bottom pixel rows).
    CHECK_EQ(ui::alertRowAt(font, dash, item, chrome.body.x, list.rows[i].y), static_cast<int>(i));
    CHECK_EQ(ui::alertRowAt(font, dash, item, chrome.body.x,
                            static_cast<int16_t>(list.rows[i].bottom() - 1)),
             static_cast<int>(i));
    CHECK(list.rows[i].contains(chrome.body.x, cy));
  }
  // Title row, "+N more" line and the frame padding are not event rows.
  if (chrome.title) {
    CHECK_EQ(ui::alertRowAt(font, dash, item, static_cast<int16_t>(chrome.title_row.x + 4),
                            static_cast<int16_t>(chrome.title_row.y + 2)),
             -1);
  }
  if (list.more) {
    CHECK_EQ(ui::alertRowAt(font, dash, item, static_cast<int16_t>(list.more_bounds.x + 20),
                            static_cast<int16_t>(list.more_bounds.y + 6)),
             -1);
  }
  CHECK_EQ(ui::alertRowAt(font, dash, item, static_cast<int16_t>(item.bounds.x + 1),
                          static_cast<int16_t>(item.bounds.y + 1)),
           -1);
}

}  // namespace

TEST(alert_hit, single_line_rows_match_drawn_markers) {
  checkRowsMatchDrawing(alertDashboard(4, 12), 4);
}

TEST(alert_hit, two_line_rows_match_drawn_markers) {
  checkRowsMatchDrawing(alertDashboard(8, 3, "Alerts", true), 3);
}

TEST(alert_hit, untitled_panel_rows_start_at_body_top) {
  checkRowsMatchDrawing(alertDashboard(3, 5, ""), 3);
}

TEST(alert_hit, more_line_is_not_a_row) {
  const Dashboard dash = alertDashboard(2, 30);
  Rendered rendered = render(dash);
  REQUIRE(rendered.layout.panels.size() == 1);
  eego::FontManager& font = eego::FontManager::instance();
  const ui::PanelLayout& item = rendered.layout.panels.front();
  const ui::PanelChrome chrome = ui::panelChrome(font, *item.panel, item.bounds);
  const ui::AlertListLayout list = ui::layoutAlertList(font, dash, *item.panel, chrome.body);
  CHECK(list.rows.size() < dash.events.size());
  checkRowsMatchDrawing(dash, 1);
}

TEST(alert_hit, display_order_is_active_by_severity_then_newest) {
  const Dashboard dash = alertDashboard(8, 6);
  const std::vector<const Event*> order = ui::alertPanelEvents(dash, dash.pages[0].panels[0]);
  REQUIRE(order.size() == 6);
  // Active criticals newest first (e5, e4, e2), then warnings (e3, e0), then cleared e1.
  CHECK_EQ(order[0]->id, std::string("e5"));
  CHECK_EQ(order[1]->id, std::string("e4"));
  CHECK_EQ(order[2]->id, std::string("e2"));
  CHECK_EQ(order[3]->id, std::string("e3"));
  CHECK_EQ(order[4]->id, std::string("e0"));
  CHECK_EQ(order[5]->id, std::string("e1"));
}

TEST(alert_hit, non_alert_panel_has_no_rows) {
  Dashboard dash = alertDashboard(4, 3);
  dash.pages[0].panels[0].kind = PanelKind::Text;
  const ui::PageLayout layout = ui::buildPageLayout(dash.pages[0], 0);
  REQUIRE(layout.panels.size() == 1);
  const ui::PanelLayout& item = layout.panels.front();
  CHECK(ui::alertEventAt(dash, item, static_cast<int16_t>(item.bounds.x + 20),
                         static_cast<int16_t>(item.bounds.y + 60)) == nullptr);
}

TEST(alert_hit, active_critical_survives_cleared_flood) {
  // 300 newer cleared events precede one older active critical in insertion
  // order: sorting must happen before the 256 cap, so the critical comes first
  // in both the draw order and the hit-test row 0.
  Dashboard dash = alertDashboard(4, 0);
  for (size_t i = 0; i < 300; ++i) {
    Event event;
    event.id = "c" + std::to_string(i);
    event.title = "Cleared " + std::to_string(i);
    event.timestamp_ms = 1700000600000LL + static_cast<int64_t>(i) * 1000;
    event.severity = Severity::Warning;
    event.state = EventState::Cleared;
    dash.events.push_back(event);
  }
  Event critical;
  critical.id = "crit";
  critical.title = "Critical";
  critical.timestamp_ms = 1700000000000LL;
  critical.severity = Severity::Critical;
  critical.state = EventState::Active;
  dash.events.push_back(critical);

  const std::vector<const Event*> order = ui::alertPanelEvents(dash, dash.pages[0].panels[0]);
  REQUIRE(order.size() == ui::kAlertListMax);
  CHECK_EQ(order[0]->id, std::string("crit"));
  CHECK_EQ(order[1]->id, std::string("c299"));  // then newest cleared first

  Rendered rendered = render(dash);
  REQUIRE(rendered.layout.panels.size() == 1);
  const ui::PanelLayout& item = rendered.layout.panels.front();
  eego::FontManager& font = eego::FontManager::instance();
  const ui::PanelChrome chrome = ui::panelChrome(font, *item.panel, item.bounds);
  const ui::AlertListLayout list = ui::layoutAlertList(font, dash, *item.panel, chrome.body);
  REQUIRE(!list.rows.empty());
  REQUIRE(list.events.size() == order.size());
  CHECK(list.events[0] == order[0]);
  const ui::Rect& row = list.rows[0];
  CHECK(ui::alertEventAt(dash, item, static_cast<int16_t>(row.x + row.w / 2),
                         static_cast<int16_t>(row.y + row.h / 2)) == order[0]);
}
