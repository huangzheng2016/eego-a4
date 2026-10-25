// SPDX-License-Identifier: GPL-3.0-or-later
// dashboard_ui layout: proportional mapping, 8 rows per screen, no overlaps,
// and strict rejection of invalid screen layouts.
#include <array>
#include <map>

#include "dashboard_core/config.h"
#include "dashboard_ui/layout.h"
#include "test_util.h"

using namespace eego::dashboard;
using namespace eego::dashboard::ui;

namespace {

PanelSpec panel(const char* id, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                bool visible = true) {
  PanelSpec p;
  p.id = id;
  p.kind = PanelKind::Text;
  p.grid = {x, y, w, h};
  p.visible = visible;
  return p;
}

Page pageOf(std::vector<PanelSpec> panels, uint32_t columns = 24) {
  Page page;
  page.id = "main";
  page.grid_columns = columns;
  page.panels = std::move(panels);
  return page;
}

bool inside(const Rect& r, const Rect& bounds) {
  return !r.empty() && r.x >= bounds.x && r.y >= bounds.y && r.right() <= bounds.right() &&
         r.bottom() <= bounds.bottom();
}

// Asserts the core layout invariants for one page; returns number of problems.
void checkPageInvariants(const std::string& name, const Page& page) {
  const PageLayout first = buildPageLayout(page, 0);
  CHECK_MSG(first.valid, name);
  std::map<size_t, size_t> seen;  // panel index -> screen
  for (size_t screen = 0; screen < first.page_count; ++screen) {
    const PageLayout layout = buildPageLayout(page, screen);
    CHECK_EQ(layout.page_count, first.page_count);
    for (size_t i = 0; i < layout.panels.size(); ++i) {
      const PanelLayout& a = layout.panels[i];
      CHECK_MSG(inside(a.bounds, kDashboardContent),
                name << " " << a.panel->id << " out of content area");
      CHECK_MSG(inside(a.content, a.bounds) || a.content.empty(), name << " " << a.panel->id);
      CHECK_MSG(a.row + a.row_span <= kRowsPerPage, name << " " << a.panel->id << " exceeds screen");
      CHECK_MSG(panelScreenIndex(*a.panel) == screen, name << " " << a.panel->id << " wrong screen");
      CHECK_MSG(seen.count(a.panel_index) == 0, name << " " << a.panel->id << " on two screens");
      seen[a.panel_index] = screen;
      for (size_t j = 0; j < i; ++j) {
        CHECK_MSG(!overlaps(a.bounds, layout.panels[j].bounds),
                  name << " screen " << screen << ": " << a.panel->id << " overlaps "
                       << layout.panels[j].panel->id);
      }
    }
  }
  for (size_t i = 0; i < page.panels.size(); ++i) {
    if (page.panels[i].visible)
      CHECK_MSG(seen.count(i) == 1, name << " visible panel " << page.panels[i].id << " not laid out");
  }
}

}  // namespace

TEST(layout, all_shipped_dashboards_have_no_overlap) {
  size_t pages = 0;
  for (const auto& bundle : hosttest::loadCorpus()) {
    const ParseResult result = dashboardFromJson(bundle.dashboard);
    if (!result.ok()) continue;  // reported by dashboard suite
    for (const Page& page : result.dashboard.pages) {
      ++pages;
      checkPageInvariants(bundle.name + "#" + page.id, page);
    }
  }
  CHECK(pages > 0);
}

TEST(layout, full_width_full_height_panel_fills_content) {
  const Page page = pageOf({panel("p", 0, 0, 24, 8)});
  const PageLayout l = buildPageLayout(page, 0);
  REQUIRE(l.panels.size() == 1);
  CHECK(l.panels[0].bounds == kDashboardContent);
  CHECK_EQ(l.page_count, size_t{1});
}

TEST(layout, adjacent_panels_separated_by_exactly_one_gap) {
  for (uint32_t columns : {24u, 12u, 7u, 5u, 3u}) {
    std::vector<PanelSpec> panels;
    static const char* ids[] = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l",
                                "m", "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x"};
    for (uint32_t c = 0; c < columns; ++c) panels.push_back(panel(ids[c], c, 0, 1, 1));
    const PageLayout l = buildPageLayout(pageOf(panels, columns), 0);
    REQUIRE(l.panels.size() == columns);
    CHECK_EQ(l.panels.front().bounds.x, kDashboardContent.x);
    CHECK_EQ(l.panels.back().bounds.right(), kDashboardContent.right());
    for (size_t i = 1; i < l.panels.size(); ++i) {
      CHECK_EQ(l.panels[i - 1].bounds.right() + 8, static_cast<int32_t>(l.panels[i].bounds.x));
      CHECK(l.panels[i].bounds.w > 0);
    }
  }
  std::vector<PanelSpec> rows;
  static const char* rid[] = {"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7"};
  for (uint32_t r = 0; r < kRowsPerPage; ++r) rows.push_back(panel(rid[r], 0, r, 24, 1));
  const PageLayout l = buildPageLayout(pageOf(rows), 0);
  REQUIRE(l.panels.size() == kRowsPerPage);
  CHECK_EQ(l.panels.back().bounds.bottom(), kDashboardContent.bottom());
  for (size_t i = 1; i < l.panels.size(); ++i)
    CHECK_EQ(l.panels[i - 1].bounds.bottom() + 8, static_cast<int32_t>(l.panels[i].bounds.y));
}

TEST(layout, eight_rows_per_screen_and_screen_index) {
  CHECK_EQ(kRowsPerPage, 8u);
  CHECK_EQ(panelScreenIndex(panel("a", 0, 0, 1, 8)), size_t{0});
  CHECK_EQ(panelScreenIndex(panel("a", 0, 7, 1, 1)), size_t{0});
  CHECK_EQ(panelScreenIndex(panel("a", 0, 8, 1, 1)), size_t{1});
  CHECK_EQ(panelScreenIndex(panel("a", 0, 17, 1, 2)), size_t{2});
  // Invalid crossing panels are pushed to the next screen, never split.
  CHECK_EQ(panelScreenIndex(panel("a", 0, 6, 1, 4)), size_t{1});

  const Page page = pageOf({panel("a", 0, 0, 24, 8), panel("b", 0, 8, 12, 4),
                            panel("c", 12, 16, 12, 8), panel("hidden", 0, 40, 4, 4, false)});
  const PageLayout s0 = buildPageLayout(page, 0);
  CHECK_EQ(s0.page_count, size_t{3});  // hidden panel does not add screens
  REQUIRE(s0.panels.size() == 1);
  const PageLayout s1 = buildPageLayout(page, 1);
  REQUIRE(s1.panels.size() == 1);
  CHECK_EQ(s1.panels[0].panel->id, std::string("b"));
  CHECK_EQ(s1.panels[0].row, 0u);
  CHECK_EQ(s1.panels[0].bounds.y, kDashboardContent.y);
  const PageLayout s2 = buildPageLayout(page, 2);
  REQUIRE(s2.panels.size() == 1);
  CHECK_EQ(s2.panels[0].bounds.bottom(), kDashboardContent.bottom());
  CHECK(buildPageLayout(page, 3).panels.empty());
}

TEST(layout, hit_test_and_find_bounds) {
  Page page = pageOf({panel("txt", 0, 0, 24, 4), panel("btn", 0, 4, 24, 4)});
  page.panels[1].kind = PanelKind::Button;
  page.panels[1].actions.resize(2);
  const PageLayout l = buildPageLayout(page, 0);
  Rect bounds;
  REQUIRE(findPanelBounds(l, "btn", bounds));
  CHECK(!findPanelBounds(l, "nope", bounds));
  HitTarget target;
  const PanelLayout& btn = l.panels[1];
  const Rect a0 = actionBounds(btn, 2, 0);
  const Rect a1 = actionBounds(btn, 2, 1);
  CHECK(!overlaps(a0, a1));
  CHECK(inside(a0, btn.content) && inside(a1, btn.content));
  REQUIRE(hitTest(l, a1.x + a1.w / 2, a1.y + a1.h / 2, target));
  CHECK_EQ(target.action_index, size_t{1});
  const Rect txt = l.panels[0].bounds;
  CHECK(!hitTest(l, txt.x + 5, txt.y + 5, target));  // not a button
}

namespace {

JsonValue gridDashboard(const std::vector<std::array<int, 4>>& grids) {
  JsonValue::Array panels;
  int n = 0;
  for (const auto& g : grids) {
    const std::string id = "p" + std::to_string(n++);
    JsonValue p;
    hosttest::parseJson(
        R"({"id":")" + id + R"(","kind":"text","grid":{"x":)" + std::to_string(g[0]) +
            R"(,"y":)" + std::to_string(g[1]) + R"(,"w":)" + std::to_string(g[2]) + R"(,"h":)" +
            std::to_string(g[3]) +
            R"(},"title":"","visible":true,"text":{"content":"x","multiline":false,"max_lines":1}})",
        p);
    panels.push_back(p);
  }
  JsonValue d;
  hosttest::parseJson(R"({"schema":"eego.dashboard/2","schema_version":2,"id":"grid","title":"L",
    "default_page_id":"main","refresh_ms":60000,"sources":[],"variables":[],
    "pages":[{"id":"main","title":"M","visible":true,"grid_columns":24,"panels":[]}],
    "alert_rules":[],"assets":[],"samples":[],"events":[]})",
                      d);
  *d.find("pages")->arrayItems()[0].find("panels") = JsonValue::makeArray(std::move(panels));
  return d;
}

}  // namespace

TEST(layout, strict_validation_rejects_bad_screen_layouts) {
  CHECK(dashboardFromJson(gridDashboard({{0, 0, 24, 6}, {0, 8, 24, 8}})).ok());
  CHECK(!dashboardFromJson(gridDashboard({{0, 0, 24, 6}, {0, 4, 12, 4}})).ok());  // overlap
  CHECK(!dashboardFromJson(gridDashboard({{12, 6, 12, 4}})).ok());  // crosses a screen
  CHECK(!dashboardFromJson(gridDashboard({{0, 8, 24, 12}})).ok());  // taller than a screen
}

TEST(layout, strict_validation_ignores_hidden_panels) {
  JsonValue d = gridDashboard({{0, 0, 24, 8}, {0, 0, 24, 8}});
  JsonValue& second = d.find("pages")->arrayItems()[0].find("panels")->arrayItems()[1];
  *second.find("visible") = JsonValue::makeBool(false);
  CHECK(dashboardFromJson(d).ok());
}
