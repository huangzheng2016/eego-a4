// SPDX-License-Identifier: GPL-3.0-or-later
// dashboard_ui layout: proportional mapping, one page = one 8-row screen, no
// overlaps, and strict rejection of invalid screen layouts.
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

// Asserts the core layout invariants for one page (= one screen).
void checkPageInvariants(const std::string& name, const Page& page) {
  const PageLayout layout = buildPageLayout(page);
  CHECK_MSG(layout.valid, name);
  std::map<size_t, size_t> seen;
  for (size_t i = 0; i < layout.panels.size(); ++i) {
    const PanelLayout& a = layout.panels[i];
    CHECK_MSG(inside(a.bounds, kDashboardContent),
              name << " " << a.panel->id << " out of content area");
    CHECK_MSG(inside(a.content, a.bounds) || a.content.empty(), name << " " << a.panel->id);
    CHECK_MSG(a.row + a.row_span <= kRowsPerPage, name << " " << a.panel->id << " exceeds screen");
    CHECK_MSG(seen.count(a.panel_index) == 0, name << " " << a.panel->id << " laid out twice");
    seen[a.panel_index] = i;
    for (size_t j = 0; j < i; ++j) {
      CHECK_MSG(!overlaps(a.bounds, layout.panels[j].bounds),
                name << ": " << a.panel->id << " overlaps " << layout.panels[j].panel->id);
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
  const PageLayout l = buildPageLayout(page);
  REQUIRE(l.panels.size() == 1);
  CHECK(l.panels[0].bounds == kDashboardContent);
}

TEST(layout, adjacent_panels_separated_by_exactly_one_gap) {
  for (uint32_t columns : {24u, 12u, 7u, 5u, 3u}) {
    std::vector<PanelSpec> panels;
    static const char* ids[] = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l",
                                "m", "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x"};
    for (uint32_t c = 0; c < columns; ++c) panels.push_back(panel(ids[c], c, 0, 1, 1));
    const PageLayout l = buildPageLayout(pageOf(panels, columns));
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
  const PageLayout l = buildPageLayout(pageOf(rows));
  REQUIRE(l.panels.size() == kRowsPerPage);
  CHECK_EQ(l.panels.back().bounds.bottom(), kDashboardContent.bottom());
  for (size_t i = 1; i < l.panels.size(); ++i)
    CHECK_EQ(l.panels[i - 1].bounds.bottom() + 8, static_cast<int32_t>(l.panels[i].bounds.y));
}

TEST(layout, one_page_is_one_eight_row_screen) {
  CHECK_EQ(kRowsPerPage, 8u);
  CHECK(gridFitsScreen({0, 0, 1, 8}));
  CHECK(gridFitsScreen({0, 7, 1, 1}));
  CHECK(!gridFitsScreen({0, 7, 1, 2}));
  CHECK(!gridFitsScreen({0, 8, 1, 1}));
  CHECK(!gridFitsScreen({0, 0, 1, 9}));

  // Panels beyond the screen (invalid input) and hidden panels are skipped.
  const Page page = pageOf({panel("a", 0, 0, 24, 4), panel("b", 0, 4, 12, 4),
                            panel("below", 0, 8, 12, 4), panel("cross", 12, 6, 12, 4),
                            panel("hidden", 12, 4, 4, 4, false)});
  const PageLayout l = buildPageLayout(page);
  REQUIRE(l.panels.size() == 2);
  CHECK_EQ(l.panels[0].panel->id, std::string("a"));
  CHECK_EQ(l.panels[1].panel->id, std::string("b"));
  CHECK_EQ(l.panels[1].row, 4u);
  CHECK_EQ(l.panels[1].bounds.bottom(), kDashboardContent.bottom());
}

TEST(layout, hit_test_and_find_bounds) {
  Page page = pageOf({panel("txt", 0, 0, 24, 4), panel("btn", 0, 4, 24, 4)});
  page.panels[1].kind = PanelKind::Button;
  page.panels[1].actions.resize(2);
  const PageLayout l = buildPageLayout(page);
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
  CHECK(dashboardFromJson(gridDashboard({{0, 0, 24, 6}, {0, 6, 24, 2}})).ok());
  CHECK(!dashboardFromJson(gridDashboard({{0, 0, 24, 6}, {0, 4, 12, 4}})).ok());  // overlap
  CHECK(!dashboardFromJson(gridDashboard({{12, 6, 12, 4}})).ok());  // below the screen
  CHECK(!dashboardFromJson(gridDashboard({{0, 8, 24, 8}})).ok());   // former 2nd screen
  CHECK(!dashboardFromJson(gridDashboard({{0, 0, 24, 9}})).ok());   // taller than a screen
}

TEST(layout, strict_validation_ignores_hidden_panels) {
  JsonValue d = gridDashboard({{0, 0, 24, 8}, {0, 0, 24, 8}});
  JsonValue& second = d.find("pages")->arrayItems()[0].find("panels")->arrayItems()[1];
  *second.find("visible") = JsonValue::makeBool(false);
  CHECK(dashboardFromJson(d).ok());
}
