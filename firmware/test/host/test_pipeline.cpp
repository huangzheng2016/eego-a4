// SPDX-License-Identifier: GPL-3.0-or-later
// data_pipeline: JSONPath subset, response parsing, history aggregation.
#include "data_pipeline/history.h"
#include "data_pipeline/json_path.h"
#include "data_pipeline/parser.h"
#include "data_pipeline/source_plan.h"
#include "test_util.h"

using namespace eego::dashboard;
using namespace eego::dashboard::pipeline;

namespace {

constexpr int64_t kNow = 1767225600000LL;  // 2026-01-01T00:00:00Z
constexpr int64_t kMin = 60000;
constexpr int64_t kHour = 60 * kMin;

JsonValue json(const std::string& text) {
  JsonValue v;
  std::string error;
  CHECK_MSG(hosttest::parseJson(text, v, &error), error);
  return v;
}

HttpResponse ok(std::string body) {
  HttpResponse r;
  r.status = 200;
  r.body = std::move(body);
  return r;
}

const MetricSample* findMetric(const SourceResult& r, const std::string& name) {
  for (const auto& m : r.metrics)
    if (m.metric == name) return &m;
  return nullptr;
}

std::string label(const MetricSample& m, const std::string& key) {
  for (const auto& kv : m.labels)
    if (kv.key == key) return kv.value;
  return "<none>";
}

MetricSample sample(const char* metric, int64_t ts, double value) {
  MetricSample s;
  s.source_id = "src";
  s.metric = metric;
  s.timestamp_ms = ts;
  s.value = value;
  return s;
}

}  // namespace

TEST(pipeline, json_path_selects_children_indices_and_wildcards) {
  const JsonValue root = json(R"({"a":{"b":[{"v":1},{"v":2},{"v":3}],"key with space":7},"n":null})");
  std::vector<const JsonValue*> out;
  std::string error;
  REQUIRE(selectJsonPath(root, "$.a.b[*].v", out, error));
  REQUIRE(out.size() == 3);
  CHECK_EQ(out[2]->intValue(), int64_t{3});
  CHECK_EQ(jsonPathFirst(root, "$.a.b[1].v")->intValue(), int64_t{2});
  CHECK_EQ(jsonPathFirst(root, "$['a']['key with space']")->intValue(), int64_t{7});
  CHECK_EQ(jsonPathFirst(root, "a.b[0].v")->intValue(), int64_t{1});
  CHECK(jsonPathFirst(root, "$.a.b[9]") == nullptr);
  CHECK(jsonPathFirst(root, "$.missing.x") == nullptr);
  CHECK(jsonPathFirst(root, "$") == &root);
  out.clear();
  REQUIRE(selectJsonPath(root, "$.a.b[*].v", out, error, 2) || !error.empty());
  CHECK(out.size() <= 2);  // result budget honoured (or reported as error)
}

TEST(pipeline, json_path_rejects_unsupported_syntax) {
  std::string error;
  CHECK(validateJsonPath("$.a[0].b", error));
  CHECK(validateJsonPath("$['x']", error));
  CHECK(validateJsonPath("a.b", error));  // implicit root is allowed
  for (const char* bad : {".a", "[0]", "$..a", "$.a[?(@.v>1)]", "$.a[0:2]", "$.a[0,1]", "$.a[",
                          "$.a[-1]", ""}) {
    CHECK_MSG(!validateJsonPath(bad, error), bad);
  }
}

TEST(pipeline, parse_timestamps) {
  int64_t ts = 0;
  CHECK(parseTimestamp(JsonValue::makeInt(1767225600), ts));
  CHECK_EQ(ts, kNow);  // seconds promoted to ms
  CHECK(parseTimestamp(JsonValue::makeInt(kNow), ts));
  CHECK_EQ(ts, kNow);
  CHECK(parseTimestamp(JsonValue::makeString("2026-01-01T00:00:00Z"), ts));
  CHECK_EQ(ts, kNow);
  CHECK(parseTimestamp(JsonValue::makeString("2026-01-01T08:00:00+08:00"), ts));
  CHECK_EQ(ts, kNow);
  CHECK(parseTimestamp(JsonValue::makeString("2026-01-01T00:00:00.250Z"), ts));
  CHECK_EQ(ts, kNow + 250);
  CHECK(!parseTimestamp(JsonValue::makeString("2026-02-30T00:00:00Z"), ts));
  CHECK(!parseTimestamp(JsonValue::makeString("2026-01-01 00:00:00"), ts));  // no zone
  CHECK(!parseTimestamp(JsonValue::makeString("yesterday"), ts));
}

TEST(pipeline, standard_json_payload) {
  SourcePlan plan;
  plan.source.id = "srv";
  plan.source.url = "http://127.0.0.1:8978/server";
  plan.response_kind = ResponseKind::StandardJson;
  const std::string body = R"({
    "metrics":[
      {"metric":"server.cpu","value":42.5,"unit":"percent","timestamp_ms":1767225540000,
       "labels":{"server":"srv-demo"}},
      {"metric":"server.cpu","value":"47","unit":"percent","timestamp_ms":1767225600000,
       "labels":{"server":"srv-demo"}}],
    "events":[{"id":"cpu","rule_id":"","timestamp_ms":1767225600000,"severity":"warning",
               "state":"active","title":"High CPU","message":"above 80%","linked_panel_id":""}],
    "text":"srv-demo: CPU high"})";
  SourceResult result;
  std::string error;
  REQUIRE_MSG(parseSourceResponse(plan, ok(body), {}, kNow, result, error), error);
  CHECK_EQ(result.source_id, std::string("srv"));
  REQUIRE(result.metrics.size() == 2);
  CHECK_NEAR(result.metrics[0].value, 42.5, 1e-12);
  CHECK_NEAR(result.metrics[1].value, 47.0, 1e-12);  // numeric strings accepted
  CHECK_EQ(result.metrics[0].unit, std::string("percent"));
  CHECK_EQ(label(result.metrics[0], "server"), std::string("srv-demo"));
  REQUIRE(result.events.size() == 1);
  CHECK(result.events[0].severity == Severity::Warning);
  CHECK(result.events[0].state == EventState::Active);
  CHECK(result.has_text);
  CHECK_EQ(result.text.text, std::string("srv-demo: CPU high"));
}

TEST(pipeline, generic_json_with_builtin_weather_plan) {
  const SourcePlan plan = makeWeatherPlan("weather", "http://127.0.0.1:8978/weather");
  std::string error;
  REQUIRE(validateSourcePlan(plan, error));
  SourceResult result;
  REQUIRE(parseSourceResponse(
      plan, ok(R"({"city":"Shanghai","temperature":21.5,"humidity":64,"wind_speed":11.8})"), {},
      kNow, result, error));
  REQUIRE(result.metrics.size() == 3);
  const MetricSample* temp = findMetric(result, "weather.temperature");
  REQUIRE(temp != nullptr);
  CHECK_NEAR(temp->value, 21.5, 1e-12);
  CHECK_EQ(temp->unit, std::string("C"));
  CHECK_EQ(label(*temp, "city"), std::string("Shanghai"));
  CHECK_EQ(temp->timestamp_ms, kNow);  // missing timestamp -> fetch time
}

TEST(pipeline, generic_json_mapping_with_expression_and_unit) {
  SourcePlan plan;
  plan.source.id = "tok";
  plan.source.url = "http://127.0.0.1/usage";
  plan.response_kind = ResponseKind::GenericJson;
  MetricMapping m;
  m.items_path = "$.data[*]";
  m.name_path = "$.name";
  m.value_path = "$.used";
  m.timestamp_path = "$.ts";
  m.unit = "MiB";
  m.target_unit = "GiB";
  plan.metrics.push_back(m);
  std::string error;
  REQUIRE(validateSourcePlan(plan, error));
  SourceResult result;
  REQUIRE_MSG(parseSourceResponse(
      plan, ok(R"({"data":[{"name":"a","used":2048,"ts":"2026-01-01T00:00:00Z"},
                            {"name":"b","used":512,"ts":1767225600}]})"),
      {}, kNow + 5000, result, error), error);
  REQUIRE(result.metrics.size() == 2);
  CHECK_NEAR(findMetric(result, "a")->value, 2.0, 1e-12);
  CHECK_NEAR(findMetric(result, "b")->value, 0.5, 1e-12);
  CHECK_EQ(findMetric(result, "a")->unit, std::string("GiB"));
  CHECK_EQ(findMetric(result, "b")->timestamp_ms, kNow);
}

TEST(pipeline, prometheus_vector) {
  SourcePlan plan;
  plan.source.id = "prom";
  plan.source.url = "http://127.0.0.1:9090/api/v1/query";
  plan.response_kind = ResponseKind::Prometheus;
  SourceResult result;
  std::string error;
  REQUIRE_MSG(parseSourceResponse(
      plan, ok(R"({"status":"success","data":{"resultType":"vector","result":[
        {"metric":{"__name__":"up","job":"node"},"value":[1767225600,"1"]},
        {"metric":{"__name__":"up","job":"db"},"value":[1767225600.5,"0"]}]}})"),
      {}, kNow, result, error), error);
  REQUIRE(result.metrics.size() == 2);
  CHECK_EQ(result.metrics[0].timestamp_ms, kNow);
  CHECK_NEAR(result.metrics[0].value, 1.0, 1e-12);
  CHECK_EQ(result.metrics[1].timestamp_ms, kNow + 500);
  CHECK(!parseSourceResponse(plan, ok(R"({"status":"error"})"), {}, kNow, result, error));
}

TEST(pipeline, rejects_bad_responses) {
  SourcePlan plan;
  plan.source.id = "s";
  plan.source.url = "http://127.0.0.1/x";
  plan.response_kind = ResponseKind::StandardJson;
  SourceResult result;
  std::string error;
  CHECK(!parseSourceResponse(plan, ok("[1,2]"), {}, kNow, result, error));  // not an object
  CHECK(!parseSourceResponse(plan, ok("{bad json"), {}, kNow, result, error));
  HttpResponse failed;
  failed.status = 503;
  CHECK(!parseSourceResponse(plan, failed, {}, kNow, result, error));
  plan.source.max_response_bytes = 8;
  CHECK(!parseSourceResponse(plan, ok(R"({"metrics":[]})"), {}, kNow, result, error));
}

TEST(pipeline, image_and_text_responses) {
  SourcePlan plan;
  plan.source.id = "img";
  plan.source.url = "http://127.0.0.1/a.png";
  plan.response_kind = ResponseKind::Image;
  SourceResult result;
  std::string error;
  CHECK(parseSourceResponse(plan, ok(std::string("\x89PNG\r\n\x1a\n....", 12)), {}, kNow, result,
                            error));
  CHECK(result.has_image && result.image.media_type == "image/png");
  CHECK(!parseSourceResponse(plan, ok("plain"), {}, kNow, result, error));
  plan.response_kind = ResponseKind::Text;
  CHECK(parseSourceResponse(plan, ok("hello"), {}, kNow, result, error));
  CHECK(result.has_text && result.text.text == "hello");
}

TEST(pipeline, aggregate_samples) {
  // Out-of-order input; NaN and negative timestamps are ignored.
  const std::vector<MetricSample> samples = {
      sample("m", kNow + 2000, 30), sample("m", kNow, 10), sample("m", kNow + 1000, 20),
      sample("m", -5, 999), sample("m", kNow + 500, std::nan(""))};
  CHECK_NEAR(aggregateSamples(samples, Aggregation::Sum).value, 60.0, 1e-12);
  CHECK_NEAR(aggregateSamples(samples, Aggregation::Avg).value, 20.0, 1e-12);
  CHECK_NEAR(aggregateSamples(samples, Aggregation::Min).value, 10.0, 1e-12);
  CHECK_NEAR(aggregateSamples(samples, Aggregation::Max).value, 30.0, 1e-12);
  CHECK_NEAR(aggregateSamples(samples, Aggregation::Delta).value, 20.0, 1e-12);
  CHECK_NEAR(aggregateSamples(samples, Aggregation::Rate).value, 10.0, 1e-12);  // per second
  const AggregationResult r = aggregateSamples(samples, Aggregation::Sum);
  CHECK_EQ(r.sample_count, size_t{3});
  CHECK_EQ(r.first_timestamp_ms, kNow);
  CHECK_EQ(r.last_timestamp_ms, kNow + 2000);
  CHECK(!aggregateSamples({}, Aggregation::Sum).valid);
  CHECK(!aggregateSamples({sample("m", kNow, 1)}, Aggregation::Rate).valid);  // no elapsed time
}

TEST(pipeline, resolve_windows) {
  const int64_t now = kNow + 13 * kHour + 7 * kMin;  // 13:07Z
  ResolvedWindow w = resolveWindow(WindowKind::Rolling24h, now);
  CHECK_EQ(w.to_ms, now);
  CHECK_EQ(w.from_ms, now - 24 * kHour);
  w = resolveWindow(WindowKind::Today, now);
  CHECK_EQ(w.from_ms, kNow);
  w = resolveWindow(WindowKind::Today, now, 0, 480);  // UTC+8: local 21:07, day began 16:00Z prev day
  CHECK_EQ(w.from_ms, kNow - 8 * kHour);
  w = resolveWindow(WindowKind::Today, kNow + 17 * kHour, 0, 480);  // local 01:00 next day
  CHECK_EQ(w.from_ms, kNow + 16 * kHour);
  w = resolveWindow(WindowKind::Fixed5h, now);  // epoch-aligned 5 h buckets
  CHECK_EQ(w.from_ms % (5 * kHour), int64_t{0});
  CHECK(w.from_ms <= now && now - w.from_ms < 5 * kHour);
  CHECK_EQ(w.to_ms, now);
  w = resolveWindow(WindowKind::Fixed7d, now);
  CHECK(w.from_ms <= now && now - w.from_ms < 7 * 24 * kHour);
  w = resolveWindow(WindowKind::Fixed5h, now, kNow + 15 * kHour);  // provider reset wins
  CHECK_EQ(w.from_ms, kNow + 10 * kHour);
  CHECK_EQ(w.to_ms, kNow + 15 * kHour);
}

TEST(pipeline, metric_history_append_query_aggregate) {
  MetricHistory history;
  for (int i = 0; i < 10; ++i) {
    SourceResult r;
    r.source_id = "src";
    r.fetched_at_ms = kNow + i * kMin;
    r.metrics.push_back(sample("cpu", kNow + i * kMin, i + 1));
    std::string error;
    CHECK_MSG(history.append(r, r.fetched_at_ms, error), error);
  }
  CHECK_EQ(history.seriesCount(), size_t{1});
  CHECK_EQ(history.rawPointCount(), size_t{10});
  const ResolvedWindow window{kNow + 2 * kMin, kNow + 5 * kMin};
  const auto points = history.query("src", "cpu", {}, window);
  CHECK(!points.empty());
  for (const auto& p : points) CHECK(p.timestamp_ms >= window.from_ms && p.timestamp_ms <= window.to_ms);
  const AggregationResult sum = history.aggregate("src", "cpu", {}, window, Aggregation::Sum);
  CHECK(sum.valid);
  CHECK_NEAR(sum.value, 3 + 4 + 5 + 6, 1e-9);
  const AggregationResult max =
      history.aggregate("src", "cpu", {}, WindowKind::Rolling24h, kNow + 10 * kMin, Aggregation::Max);
  CHECK(max.valid);
  CHECK_NEAR(max.value, 10.0, 1e-9);
  CHECK(!history.aggregate("src", "other", {}, window, Aggregation::Sum).valid);

  history.prune(kNow + 9 * 24 * kHour);  // beyond 8-day retention
  CHECK_EQ(history.rawPointCount(), size_t{0});
  history.clearSource("src");
  CHECK_EQ(history.seriesCount(), size_t{0});
}

TEST(pipeline, metric_history_respects_limits) {
  HistoryLimits limits;
  limits.max_series = 2;
  MetricHistory history(limits);
  std::string error;
  for (const char* name : {"a", "b", "c", "d"}) {
    SourceResult r;
    r.source_id = "src";
    r.metrics.push_back(sample(name, kNow, 1));
    history.append(r, kNow, error);
  }
  CHECK(history.seriesCount() <= 2);
}

TEST(pipeline, downsample_preserves_bounds) {
  std::vector<HistoryEntry> entries;
  for (int i = 0; i < 100; ++i) {
    HistoryEntry e;
    e.sample = sample("m", kNow + i * 1000, i);
    entries.push_back(e);
  }
  const AggregationResult before = aggregateHistoryEntries(entries, Aggregation::Sum);
  downsampleEntries(entries, 10);
  CHECK(entries.size() <= 10);
  const AggregationResult after = aggregateHistoryEntries(entries, Aggregation::Sum);
  CHECK_NEAR(after.value, before.value, 1e-6);  // buckets keep sum
  CHECK_EQ(after.sample_count, size_t{100});
  CHECK_NEAR(aggregateHistoryEntries(entries, Aggregation::Max).value, 99.0, 1e-9);
  CHECK_NEAR(aggregateHistoryEntries(entries, Aggregation::Min).value, 0.0, 1e-9);
}
