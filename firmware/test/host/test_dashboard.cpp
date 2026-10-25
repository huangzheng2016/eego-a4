// SPDX-License-Identifier: GPL-3.0-or-later
// dashboard_core: every shipped dashboard parses, validates, round-trips and
// exports; malformed inputs are rejected.
#include <set>

#include "dashboard_core/config.h"
#include "data_pipeline/source_plan.h"
#include "test_util.h"

namespace {
#include "dashboard_templates.inc"
}  // namespace

using namespace eego::dashboard;
using hosttest::NamedBundle;

namespace {

std::string describe(const std::vector<ValidationError>& errors) {
  std::string out;
  for (const auto& e : errors) out += "\n      " + e.field + ": " + e.message;
  return out;
}

JsonValue validDashboardJson() {
  // Small hand-written dashboard used as a base for negative cases.
  const char* text = R"({
    "schema":"eego.dashboard/2","schema_version":2,"id":"t","title":"T",
    "default_page_id":"main","refresh_ms":60000,"sources":[],"variables":[],
    "pages":[{"id":"main","title":"Main","visible":true,"grid_columns":24,
      "panels":[
        {"id":"a","kind":"text","grid":{"x":0,"y":0,"w":12,"h":4},"title":"A","visible":true,
         "text":{"content":"a","multiline":false,"max_lines":1}},
        {"id":"b","kind":"text","grid":{"x":12,"y":0,"w":12,"h":4},"title":"B","visible":true,
         "text":{"content":"b","multiline":false,"max_lines":1}}]}],
    "alert_rules":[],"assets":[],"samples":[],"events":[]})";
  JsonValue root;
  hosttest::parseJson(text, root);
  return root;
}

JsonValue& panelGrid(JsonValue& dashboard, size_t panel) {
  JsonValue& page = dashboard.find("pages")->arrayItems()[0];
  return *page.find("panels")->arrayItems()[panel].find("grid");
}

void setGrid(JsonValue& dashboard, size_t panel, int x, int y, int w, int h) {
  JsonValue& grid = panelGrid(dashboard, panel);
  *grid.find("x") = JsonValue::makeInt(x);
  *grid.find("y") = JsonValue::makeInt(y);
  *grid.find("w") = JsonValue::makeInt(w);
  *grid.find("h") = JsonValue::makeInt(h);
}

bool hasErrorContaining(const ParseResult& result, const std::string& needle) {
  for (const auto& e : result.errors)
    if (e.message.find(needle) != std::string::npos || e.field.find(needle) != std::string::npos)
      return true;
  return false;
}

}  // namespace

TEST(dashboard, corpus_is_not_empty) {
  const auto corpus = hosttest::loadCorpus();
  // 4 examples + 23 panel presets + 7 layouts + builtin sample at time of writing.
  CHECK_MSG(corpus.size() >= 30, "only " << corpus.size() << " dashboards found");
  std::set<std::string> names;
  for (const auto& b : corpus) names.insert(b.name);
  CHECK(names.count("builtin:sampleJson") == 1);
  CHECK(names.count("examples/weather.json") == 1);
}

TEST(dashboard, all_shipped_dashboards_parse_and_validate) {
  for (const NamedBundle& bundle : hosttest::loadCorpus()) {
    const ParseResult result = dashboardFromJson(bundle.dashboard);
    CHECK_MSG(result.ok(), bundle.name << describe(result.errors));
    if (!result.ok()) continue;
    std::vector<ValidationError> errors;
    CHECK_MSG(validateDashboard(result.dashboard, Limits{}, errors),
              bundle.name << describe(errors));
    CHECK_MSG(!result.dashboard.pages.empty(), bundle.name);
  }
}

TEST(dashboard, all_shipped_source_plans_parse_and_validate) {
  size_t plans = 0;
  for (const NamedBundle& bundle : hosttest::loadCorpus()) {
    if (bundle.source_plans.type() != JsonValue::Type::Array) continue;
    for (const JsonValue& json : bundle.source_plans.arrayItems()) {
      pipeline::SourcePlan plan;
      std::string error;
      ++plans;
      CHECK_MSG(pipeline::sourcePlanFromJson(json, plan, error), bundle.name << ": " << error);
      CHECK_MSG(pipeline::validateSourcePlan(plan, error), bundle.name << ": " << error);
    }
  }
  CHECK(plans > 0);
}

TEST(dashboard, serialize_round_trip_is_stable) {
  for (const NamedBundle& bundle : hosttest::loadCorpus()) {
    const ParseResult first = dashboardFromJson(bundle.dashboard);
    if (!first.ok()) continue;  // reported by the validation test
    const std::string once = dashboardToJson(first.dashboard).toString();
    const ParseResult second = parseDashboard(once);
    CHECK_MSG(second.ok(), bundle.name << describe(second.errors));
    if (!second.ok()) continue;
    CHECK_MSG(dashboardToJson(second.dashboard).toString() == once,
              bundle.name << ": serialization not idempotent");
    std::string exported;
    std::vector<ValidationError> errors;
    CHECK_MSG(exportDashboard(second.dashboard, exported, errors), bundle.name << describe(errors));
  }
}

TEST(dashboard, embedded_template_library_matches_presets_index) {
  JsonValue library;
  std::string error;
  REQUIRE(hosttest::parseJson(kDashboardTemplatesJson, library, &error));
  const JsonValue* rows = library.find("grid_rows_per_screen");
  REQUIRE(rows != nullptr);
  CHECK_EQ(rows->intValue(), static_cast<int64_t>(kGridRowsPerScreen));

  JsonValue index;
  REQUIRE(hosttest::parseJson(hosttest::slurp(hosttest::firmwareRoot() / "presets/index.json"),
                              index, &error));
  std::set<std::string> want, got;
  for (const JsonValue& entry : index.find("presets")->arrayItems())
    want.insert(entry.find("id")->stringValue());
  for (const JsonValue& preset : library.find("presets")->arrayItems()) {
    const std::string id = preset.find("id")->stringValue();
    got.insert(id);
    const NamedBundle bundle = hosttest::bundleFromPreset("embedded:" + id, preset);
    const ParseResult result = dashboardFromJson(bundle.dashboard);
    CHECK_MSG(result.ok(), bundle.name << describe(result.errors));
  }
  CHECK(want == got);
}

TEST(dashboard, base_fixture_is_valid) {
  const ParseResult result = dashboardFromJson(validDashboardJson());
  CHECK_MSG(result.ok(), describe(result.errors));
}

TEST(dashboard, rejects_overlapping_panels) {
  JsonValue d = validDashboardJson();
  setGrid(d, 1, 6, 0, 12, 4);
  const ParseResult result = dashboardFromJson(d);
  CHECK(!result.ok());
  CHECK(hasErrorContaining(result, "overlaps"));
}

TEST(dashboard, rejects_panel_crossing_screen_boundary) {
  JsonValue d = validDashboardJson();
  setGrid(d, 1, 12, 6, 12, 4);  // rows 6..9 cross the 8-row screen edge
  const ParseResult result = dashboardFromJson(d);
  CHECK(!result.ok());
  CHECK(hasErrorContaining(result, "screen boundary"));
}

TEST(dashboard, rejects_panel_taller_than_screen) {
  JsonValue d = validDashboardJson();
  setGrid(d, 1, 12, 0, 12, 9);
  const ParseResult result = dashboardFromJson(d);
  CHECK(!result.ok());
  CHECK(hasErrorContaining(result, "taller than one screen"));
}

TEST(dashboard, accepts_panel_on_second_screen) {
  JsonValue d = validDashboardJson();
  setGrid(d, 1, 0, 8, 24, 8);
  const ParseResult result = dashboardFromJson(d);
  CHECK_MSG(result.ok(), describe(result.errors));
}

TEST(dashboard, rejects_panel_wider_than_grid) {
  JsonValue d = validDashboardJson();
  setGrid(d, 1, 12, 0, 13, 4);
  CHECK(!dashboardFromJson(d).ok());
}

TEST(dashboard, rejects_unknown_fields) {
  JsonValue d = validDashboardJson();
  d.objectItems().emplace_back("bogus", JsonValue::makeInt(1));
  CHECK(!dashboardFromJson(d).ok());
}

TEST(dashboard, rejects_duplicate_panel_ids) {
  JsonValue d = validDashboardJson();
  JsonValue& page = d.find("pages")->arrayItems()[0];
  *page.find("panels")->arrayItems()[1].find("id") = JsonValue::makeString("a");
  CHECK(!dashboardFromJson(d).ok());
}

TEST(dashboard, rejects_bad_schema_version) {
  JsonValue d = validDashboardJson();
  *d.find("schema_version") = JsonValue::makeInt(7);
  *d.find("schema") = JsonValue::makeString("eego.dashboard/7");
  CHECK(!dashboardFromJson(d).ok());
}

TEST(dashboard, accepts_only_current_schema) {
  JsonValue v1 = validDashboardJson();
  *v1.find("schema_version") = JsonValue::makeInt(1);
  *v1.find("schema") = JsonValue::makeString("eego.dashboard/1");
  CHECK(!dashboardFromJson(v1).ok());
  JsonValue missing = validDashboardJson();
  auto& items = missing.objectItems();
  items.erase(items.begin());  // drop "schema"
  CHECK(!dashboardFromJson(missing).ok());
  JsonValue row_height = validDashboardJson();
  row_height.find("pages")->arrayItems()[0].objectItems().emplace_back(
      "row_height", JsonValue::makeInt(16));
  CHECK(!dashboardFromJson(row_height).ok());  // removed field is now unknown
}

TEST(dashboard, strict_json_parser) {
  JsonValue v;
  JsonParseError err;
  CHECK(jsonParse(R"({"a":1})", v, err));
  CHECK(!jsonParse(R"({"a":1,})", v, err));
  CHECK(!jsonParse(R"({"a":1,"a":2})", v, err));
  CHECK(!jsonParse(R"({"a":1} x)", v, err));
  CHECK(!jsonParse(R"([NaN])", v, err));
  CHECK(!jsonParse("\"\xff\"", v, err));
  CHECK(!jsonParse(R"("\ud800")", v, err));  // lone surrogate
  CHECK(jsonParse(R"("😀")", v, err));
  CHECK(!jsonParse("[[[[1]]]]", v, err, 3));
  CHECK(jsonParse("[[[1]]]", v, err, 3));
  CHECK(jsonParse(R"({"n":-12.5e2,"i":9007199254740993})", v, err));
  CHECK_NEAR(v.find("n")->doubleValue(), -1250.0, 1e-9);
  CHECK_EQ(v.find("i")->intValue(), int64_t{9007199254740993LL});
  CHECK_EQ(jsonCodePointCount("aé漢"), size_t{3});
  CHECK_EQ(jsonEscape("a\"\n"), std::string("a\\\"\\n"));
}

TEST(dashboard, rejects_oversized_json) {
  ParseOptions options;
  options.limits.max_json_bytes = 64;
  const std::string text = validDashboardJson().toString();
  CHECK(!parseDashboard(text, options).ok());
  Limits loose;
  loose.max_json_bytes = 1024 * 1024;  // larger than canonical budget -> rejected
  ValidationError error;
  CHECK(!validateLimits(loose, error));
}
