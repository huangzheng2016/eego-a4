// SPDX-License-Identifier: GPL-3.0-or-later
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "data_pipeline/cache.h"
#include "data_pipeline/json_path.h"
#include "data_pipeline/parser.h"
#include "data_pipeline/pipeline.h"
#include "data_pipeline/source_plan.h"

namespace {

using eego::dashboard::Event;
using eego::dashboard::EventState;
using eego::dashboard::KeyValue;
using eego::dashboard::MetricSample;
using eego::dashboard::Page;
using eego::dashboard::PanelKind;
using eego::dashboard::PanelSpec;
using eego::dashboard::Severity;
using eego::dashboard::SourceKind;
using eego::dashboard::Variable;
using eego::dashboard::VariableKind;
using eego::dashboard::VariableValues;
using eego::dashboard::JsonValue;
using eego::dashboard::pipeline::Aggregation;
using eego::dashboard::pipeline::AggregationResult;
using eego::dashboard::pipeline::CacheLimits;
using eego::dashboard::pipeline::DataPipeline;
using eego::dashboard::pipeline::EventMapping;
using eego::dashboard::pipeline::HistoryEntry;
using eego::dashboard::pipeline::HistoryLimits;
using eego::dashboard::pipeline::HttpResponse;
using eego::dashboard::pipeline::IHttpTransport;
using eego::dashboard::pipeline::MetricHistory;
using eego::dashboard::pipeline::MetricMapping;
using eego::dashboard::pipeline::PanelBinding;
using eego::dashboard::pipeline::PipelineConfig;
using eego::dashboard::pipeline::ResolvedWindow;
using eego::dashboard::pipeline::ResponseKind;
using eego::dashboard::pipeline::SdCache;
using eego::dashboard::pipeline::SourcePlan;
using eego::dashboard::pipeline::SourceResult;
using eego::dashboard::pipeline::SourceStatus;
using eego::dashboard::pipeline::TickResult;
using eego::dashboard::pipeline::WindowKind;

class Failure : public std::runtime_error {
 public:
  Failure(const char* expression, int line)
      : std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression) {}
};

#define CHECK(expression) \
  do {                    \
    if (!(expression)) throw Failure(#expression, __LINE__); \
  } while (false)

void checkNear(double actual, double expected, double tolerance = 1e-9) {
  if (std::fabs(actual - expected) > tolerance) {
    throw std::runtime_error("expected " + std::to_string(expected) + ", got " +
                             std::to_string(actual));
  }
}

SourcePlan plan(const std::string& id, ResponseKind kind = ResponseKind::StandardJson) {
  SourcePlan plan;
  plan.source.id = id;
  plan.source.url = "https://source.test/data";
  plan.response_kind = kind;
  return plan;
}

SourceResult parse(const SourcePlan& source_plan, const std::string& body,
                   int64_t now_ms = 1760000000000LL,
                   const VariableValues& variables = {}) {
  HttpResponse response;
  response.status = 200;
  response.body = body;
  if (source_plan.response_kind == ResponseKind::Image)
    response.headers["content-type"] = "image/png";
  SourceResult result;
  std::string error;
  CHECK(eego::dashboard::pipeline::parseSourceResponse(source_plan, response, variables, now_ms,
                                                       result, error));
  return result;
}

int64_t iso(const std::string& text) {
  int64_t timestamp = 0;
  CHECK(eego::dashboard::pipeline::parseTimestamp(JsonValue::makeString(text), timestamp));
  return timestamp;
}

MetricSample sample(const std::string& source, const std::string& metric, int64_t timestamp,
                    double value) {
  MetricSample sample;
  sample.source_id = source;
  sample.metric = metric;
  sample.timestamp_ms = timestamp;
  sample.value = value;
  return sample;
}

class MockTransport final : public IHttpTransport {
 public:
  HttpResponse response;
  int calls = 0;
  std::vector<eego::net::NetRequest> requests;
  std::function<void(const eego::net::NetRequest&, HttpResponse&)> responder;

  bool execute(const eego::net::NetRequest& request, HttpResponse& out) override {
    ++calls;
    requests.push_back(request);
    out = response;
    if (responder) responder(request, out);
    return out.status != -1;
  }
};

PanelSpec panel(const std::string& id, const std::string& source, const std::string& metric,
                uint32_t refresh_ms = 60000) {
  PanelSpec panel;
  panel.id = id;
  panel.kind = PanelKind::Chart;
  panel.source_id = source;
  panel.metric = metric;
  panel.refresh_ms = refresh_ms;
  return panel;
}

eego::dashboard::Dashboard dashboard(const std::vector<PanelSpec>& panels) {
  eego::dashboard::Dashboard dashboard;
  dashboard.id = "dashboard";
  Page page;
  page.id = "main";
  page.panels = panels;
  dashboard.default_page_id = page.id;
  dashboard.pages.push_back(std::move(page));
  return dashboard;
}

class TempDirectory {
 public:
  TempDirectory() {
    std::string pattern = "/tmp/eego-data-pipeline-XXXXXX";
    std::vector<char> path(pattern.begin(), pattern.end());
    path.push_back('\0');
    const char* created = mkdtemp(path.data());
    CHECK(created != nullptr);
    path_ = created;
  }
  ~TempDirectory() { std::filesystem::remove_all(path_); }
  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

void testJsonPathAndRequestPlan() {
  JsonValue root;
  eego::dashboard::JsonParseError parse_error;
  CHECK(eego::dashboard::jsonParse(
      R"({"data":{"items":[{"a.b":7},{"a.b":9}]}})", root, parse_error));
  std::vector<const JsonValue*> selected;
  std::string error;
  CHECK(eego::dashboard::pipeline::selectJsonPath(root, "$.data.items[*]['a.b']", selected,
                                                  error));
  CHECK(selected.size() == 2);
  CHECK(selected[0]->intValue() == 7);
  CHECK(!eego::dashboard::pipeline::validateJsonPath("$..value", error));
  CHECK(!eego::dashboard::pipeline::validateJsonPath("$.items[?(@.x)]", error));

  SourcePlan source_plan;
  CHECK(eego::dashboard::pipeline::parseSourcePlan(
      R"({
        "id":"token","url":"https://api.test/${account}","method":"POST",
        "body":"{\"account\":\"${account}\"}","response":"generic_json",
        "auth_token_ref":"nvs:token","headers":{"X-Account":"${account}"},
        "metrics":[{"items_path":"$.rows[*]","name":"token.total","value_path":"$.value"}]
      })",
      source_plan, error));
  eego::net::NetRequest request;
  const VariableValues variables = {{"account", "a1"}};
  CHECK(eego::dashboard::pipeline::buildSourceRequest(
      source_plan, variables, [](const std::string&) { return std::string("secret"); }, request,
      error));
  CHECK(request.url == "https://api.test/a1");
  CHECK(request.body == "{\"account\":\"a1\"}");
  CHECK(request.headers.size() == 2);
  CHECK(request.headers[1].value == "Bearer secret");
  CHECK(eego::dashboard::pipeline::sourceRequestKey(request) ==
        eego::dashboard::pipeline::sourceRequestKey(request));
  eego::net::NetRequest changed_request = request;
  changed_request.timeout_ms++;
  CHECK(eego::dashboard::pipeline::sourceRequestKey(request) !=
        eego::dashboard::pipeline::sourceRequestKey(changed_request));
  changed_request = request;
  changed_request.conditional_get = !changed_request.conditional_get;
  CHECK(eego::dashboard::pipeline::sourceRequestKey(request) !=
        eego::dashboard::pipeline::sourceRequestKey(changed_request));
  CHECK(!eego::dashboard::pipeline::buildSourceRequest(source_plan, {}, {}, request, error));
}

void testGenericMappingsAndPresets() {
  SourcePlan custom = plan("custom", ResponseKind::GenericJson);
  MetricMapping mapping;
  mapping.name = "weather.temperature_f";
  mapping.items_path = "$.rows[*]";
  mapping.value_path = "$.temp";
  mapping.timestamp_path = "$.ts";
  mapping.unit = "C";
  mapping.target_unit = "F";
  mapping.expression = "value * 2 + raw";
  mapping.fields = {{"raw", "$.temp"}};
  mapping.labels = {{"site", "$.site", ""}, {"city", "", "${city}"}};
  custom.metrics.push_back(mapping);
  SourceResult result = parse(custom,
                              R"({"rows":[{"temp":2,"ts":1760000000,"site":"a"}]})",
                              1760000000000LL, {{"city", "Paris"}});
  CHECK(result.metrics.size() == 1);
  checkNear(result.metrics[0].value, 42.8);
  CHECK(result.metrics[0].unit == "F");
  CHECK(result.metrics[0].timestamp_ms == 1760000000000LL);
  CHECK(result.metrics[0].labels.size() == 2);
  CHECK(result.metrics[0].labels[1].value == "Paris");

  SourcePlan token = eego::dashboard::pipeline::makeTokenUsagePlan("token", "https://token.test");
  result = parse(token, R"({
    "reset_at":"2026-10-21T18:00:00Z",
    "metrics":[
      {"metric":"token.5h","value":1200,"timestamp":"2026-10-21T13:00:00Z",
       "unit":"tokens","reset_at":"2026-10-21T17:00:00Z"},
      {"metric":"token.7d","value":9000,"timestamp_ms":1760000000000,"unit":"tokens"}
    ]
  })");
  CHECK(result.metrics.size() == 2);
  CHECK(result.resetAtFor("token.5h") == iso("2026-10-21T17:00:00Z"));
  CHECK(result.resetAtFor("token.7d") == iso("2026-10-21T18:00:00Z"));

  SourcePlan weather =
      eego::dashboard::pipeline::makeWeatherPlan("weather", "https://weather.test");
  result = parse(weather, R"({"temperature":20,"humidity":50,"wind_speed":12,"city":"Paris"})");
  CHECK(result.metrics.size() == 3);
  CHECK(result.metrics[0].labels[0].value == "Paris");

  SourcePlan security =
      eego::dashboard::pipeline::makeSecurityEventsPlan("security", "https://security.test");
  result = parse(security, R"({"events":[{
    "id":"sec:1","severity":"error","state":"resolved","title":"Intrusion",
    "message":"blocked","timestamp":"2026-10-21T12:00:00Z"
  }]})");
  CHECK(result.events.size() == 1);
  CHECK(result.events[0].id.rfind("ev-", 0) == 0);
  CHECK(result.events[0].severity == Severity::Critical);
  CHECK(result.events[0].state == EventState::Cleared);
}

void testStandardAndPrometheus() {
  SourceResult result = parse(plan("standard"), R"({
    "reset_at":1760000100000,
    "metrics":[{"source_id":"other","metric":"cpu","timestamp_ms":1760000000000,
                "value":0.5,"unit":"ratio","labels":{"host":"a"}}],
    "events":[{"id":"event1","timestamp_ms":1760000000000,"severity":"warning",
               "state":"active","title":"High CPU","message":"above threshold"}],
    "text":"ready"
  })");
  CHECK(result.metrics.size() == 1);
  CHECK(result.metrics[0].source_id == "standard");
  CHECK(result.metrics[0].labels[0].key == "host");
  CHECK(result.events.size() == 1);
  CHECK(result.events[0].source_id == "standard");
  CHECK(result.has_text && result.text.text == "ready");
  CHECK(result.resetAtFor("anything") == 1760000100000LL);

  result = parse(plan("small"), R"({
    "metrics":[{"metric":"m","timestamp_ms":1000,"value":1}],
    "events":[{"id":"small","timestamp_ms":1000,"title":"small"}]
  })");
  CHECK(result.metrics[0].timestamp_ms == 1000);
  CHECK(result.events[0].timestamp_ms == 1000);

  SourcePlan vector = eego::dashboard::pipeline::makeServerMetricsPlan("prom", "https://prom.test");
  result = parse(vector, R"({"status":"success","data":{"resultType":"vector","result":[{
    "metric":{"__name__":"node_cpu_seconds_total","cpu":"0","host":"a"},
    "value":[1760000000.5,"2.5"]
  }]}})");
  CHECK(result.metrics.size() == 1);
  CHECK(result.metrics[0].metric == "node_cpu_seconds_total");
  CHECK(result.metrics[0].timestamp_ms == 1760000000500LL);
  checkNear(result.metrics[0].value, 2.5);
  CHECK(result.metrics[0].labels.size() == 2);

  result = parse(vector, R"({"status":"success","data":{"resultType":"matrix","result":[{
    "metric":{"__name__":"node_memory_bytes","host":"a"},
    "values":[[1760000000,"10"],[1760000060,"20"]]
  }]}})");
  CHECK(result.metrics.size() == 2);
  CHECK(result.metrics[1].timestamp_ms == 1760000060000LL);
  checkNear(result.metrics[1].value, 20.0);

  MetricMapping scalar_mapping;
  scalar_mapping.name = "scalar";
  scalar_mapping.unit = "B";
  scalar_mapping.target_unit = "KiB";
  scalar_mapping.expression = "value * 2";
  vector.metrics.push_back(scalar_mapping);
  result = parse(vector, R"({"status":"success","data":{"resultType":"scalar",
                         "result":[1760000000,"1024"]}})");
  CHECK(result.metrics.size() == 1);
  checkNear(result.metrics[0].value, 2.0);
  CHECK(result.metrics[0].unit == "KiB");

  result = parse(vector, R"({"status":"success","data":{"resultType":"string",
                         "result":[1760000000,"healthy"]}})");
  CHECK(result.has_text && result.text.text == "healthy");

  HttpResponse response;
  response.status = 200;
  response.body = R"({"status":"error","error":"bad query"})";
  std::string error;
  CHECK(!eego::dashboard::pipeline::parseSourceResponse(vector, response, {}, 1, result, error));
}

void testTextAndImage() {
  SourceResult result = parse(plan("text", ResponseKind::Text), "hello \xe4\xb8\x96\xe7\x95\x8c");
  CHECK(result.has_text);
  CHECK(result.text.text.find("hello") == 0);

  const std::string png("\x89PNG\r\n\x1a\npayload", 12);
  result = parse(plan("image", ResponseKind::Image), png);
  CHECK(result.has_image);
  CHECK(result.image.media_type == "image/png");
  CHECK(result.image.bytes.size() == png.size());

  HttpResponse invalid;
  invalid.status = 200;
  invalid.body = std::string("\xff", 1);
  SourceResult output;
  std::string error;
  CHECK(!eego::dashboard::pipeline::parseSourceResponse(plan("text", ResponseKind::Text), invalid,
                                                        {}, 1, output, error));
}

void testWindowsAndAggregation() {
  const int64_t now = 1760000000000LL;
  ResolvedWindow window = eego::dashboard::pipeline::resolveWindow(WindowKind::Rolling24h, now);
  CHECK(window.from_ms == now - 86400000LL && window.to_ms == now);
  const int64_t reset = now + 3600000;
  window = eego::dashboard::pipeline::resolveWindow(WindowKind::Fixed5h, now, reset);
  CHECK(window.from_ms == reset - 18000000LL && window.to_ms == reset);
  window = eego::dashboard::pipeline::resolveWindow(WindowKind::Fixed7d, now, reset);
  CHECK(window.from_ms == reset - 604800000LL);
  const int64_t local_now = iso("2026-10-21T13:00:00+08:00");
  window = eego::dashboard::pipeline::resolveWindow(WindowKind::Today, local_now, 0, 480);
  CHECK(window.from_ms == iso("2026-10-21T00:00:00+08:00"));
  window = eego::dashboard::pipeline::resolveWindow(WindowKind::Today, local_now, 0, -480);
  CHECK(window.from_ms == iso("2026-10-20T00:00:00-08:00"));
  int64_t out_of_range = 0;
  CHECK(!eego::dashboard::pipeline::parseTimestamp(
      JsonValue::makeString("9223372036854775808"), out_of_range));

  std::vector<MetricSample> samples = {sample("s", "m", 3000, 2),
                                       sample("s", "m", 1000, 1),
                                       sample("s", "m", 2000, 3)};
  checkNear(eego::dashboard::pipeline::aggregateSamples(samples, Aggregation::Sum).value, 6);
  checkNear(eego::dashboard::pipeline::aggregateSamples(samples, Aggregation::Avg).value, 2);
  checkNear(eego::dashboard::pipeline::aggregateSamples(samples, Aggregation::Min).value, 1);
  checkNear(eego::dashboard::pipeline::aggregateSamples(samples, Aggregation::Max).value, 3);
  checkNear(eego::dashboard::pipeline::aggregateSamples(samples, Aggregation::Delta).value, 1);
  AggregationResult rate = eego::dashboard::pipeline::aggregateSamples(samples, Aggregation::Rate);
  CHECK(rate.valid);
  checkNear(rate.value, 0.5);
  CHECK(!eego::dashboard::pipeline::aggregateSamples(
             {sample("s", "m", 1000, 1)}, Aggregation::Rate)
             .valid);
}

void testHistoryBoundsAndResetAggregation() {
  HistoryLimits limits;
  limits.max_series = 2;
  limits.max_points_per_series = 8;
  limits.downsample_points = 4;
  limits.max_total_points = 6;
  limits.retention_ms = 1000000;
  MetricHistory history(limits);
  SourceResult result;
  result.source_id = "s";
  for (size_t i = 0; i < 10; ++i)
    result.metrics.push_back(sample("s", "a", static_cast<int64_t>(1000 + i * 1000), i));
  std::string error;
  CHECK(history.append(result, 20000, error));
  CHECK(history.pointCount() < 10 && history.pointCount() <= 6);
  CHECK(history.seriesCount() == 1);
  checkNear(history.entries().back().sample.value, 9);

  SourceResult duplicate;
  duplicate.source_id = "duplicate";
  duplicate.metrics = {sample("duplicate", "m", 1000, 1),
                       sample("duplicate", "m", 1000, 5)};
  MetricHistory single;
  CHECK(single.append(duplicate, 2000, error));
  CHECK(single.pointCount() == 1);
  checkNear(single.entries()[0].sample.value, 5);

  const int64_t now = 1760000000000LL;
  const int64_t reset = now + 3600000;
  SourceResult windowed;
  windowed.source_id = "token";
  windowed.reset_at_ms["token.5h"] = reset;
  windowed.metrics = {sample("token", "token.5h", reset - 18000000 + 1000, 2),
                      sample("token", "token.5h", reset - 18000000 - 1000, 100),
                      sample("token", "token.5h", now, 3)};
  MetricHistory with_reset;
  CHECK(with_reset.append(windowed, now, error));
  AggregationResult sum = with_reset.aggregate("token", "token.5h", {}, WindowKind::Fixed5h, now,
                                               Aggregation::Sum);
  CHECK(sum.valid);
  checkNear(sum.value, 5);
}

void testCacheRoundTripAndLimits() {
  TempDirectory temporary;
  CacheLimits limits;
  limits.max_history_points = 8;
  limits.downsample_points = 4;
  limits.max_history_bytes = 20000;
  limits.max_snapshot_bytes = 65536;
  limits.max_image_bytes = 1024;
  SdCache cache(temporary.path(), limits);
  SourceResult result;
  result.source_id = "token";
  result.fetched_at_ms = 1760000000000LL;
  result.reset_at_ms["token"] = 1760000100000LL;
  std::vector<HistoryEntry> entries;
  for (size_t i = 0; i < 12; ++i) {
    MetricSample value = sample("token", "token", 1760000000000LL + i * 1000, i);
    result.metrics.push_back(value);
    entries.push_back({value, result.reset_at_ms["token"]});
  }
  Event event;
  event.id = "event1";
  event.source_id = "token";
  event.timestamp_ms = result.fetched_at_ms;
  event.title = "budget";
  result.events.push_back(event);
  result.has_text = true;
  result.text.text = "cached";
  result.text.fetched_at_ms = result.fetched_at_ms;
  result.has_image = true;
  result.image.media_type = "image/png";
  const std::string png("\x89PNG\r\n\x1a\npayload", 12);
  result.image.bytes.assign(png.begin(), png.end());
  result.image.fetched_at_ms = result.fetched_at_ms;

  std::string error;
  CHECK(cache.appendHistory("token", entries, error));
  CHECK(cache.appendHistory("token", entries, error));
  std::vector<HistoryEntry> loaded;
  CHECK(cache.loadHistory("token", loaded, error));
  CHECK(!loaded.empty() && loaded.size() <= 8);
  CHECK(std::filesystem::file_size(temporary.path() + "/token.history.jsonl") <=
        limits.max_history_bytes);

  CHECK(cache.writeSnapshot(result, error));
  SourceResult restored;
  CHECK(cache.readSnapshot("token", restored, error));
  CHECK(restored.metrics.size() == 12);
  CHECK(restored.events.size() == 1);
  CHECK(restored.has_text && restored.text.text == "cached");
  CHECK(restored.has_image && restored.image.bytes == result.image.bytes);
  CHECK(restored.resetAtFor("token") == 1760000100000LL);

  {
    std::ofstream image(temporary.path() + "/token.image", std::ios::binary | std::ios::trunc);
    image << "bad";
  }
  CHECK(!cache.readSnapshot("token", restored, error));
  CHECK(cache.writeSnapshot(result, error));

  {
    std::ofstream history(temporary.path() + "/token.history.jsonl", std::ios::app);
    history << "torn-json";
  }
  CHECK(cache.loadHistory("token", loaded, error));
  CHECK(!loaded.empty());
}

void testSchedulerDedupFanoutAndFailure() {
  MockTransport transport;
  transport.response.status = 200;
  transport.response.body = R"({"metrics":[
    {"metric":"cpu","timestamp_ms":1760000000000,"value":10},
    {"metric":"mem","timestamp_ms":1760000000000,"value":20}
  ]})";
  SourcePlan first = plan("s1");
  SourcePlan second = plan("s2");
  DataPipeline pipeline(transport);
  std::string error;
  CHECK(pipeline.configure(dashboard({panel("p1", "s1", "cpu"), panel("p2", "s1", "mem"),
                                      panel("p3", "s2", "cpu")}),
                           {first, second}, nullptr, error));
  PanelBinding binding;
  binding.panel_id = "p1";
  binding.source_id = "s1";
  binding.metric = "cpu";
  binding.has_aggregation = true;
  binding.aggregation = Aggregation::Avg;
  CHECK(pipeline.setPanelBinding(binding, error));
  const int64_t now = 1760000000000LL;
  TickResult tick = pipeline.tick(now);
  CHECK(tick.due_sources == 2);
  CHECK(tick.http_requests == 1 && tick.deduplicated_requests == 1);
  CHECK(tick.succeeded == 2 && tick.failed == 0);
  CHECK(transport.calls == 1);
  CHECK(pipeline.takeDirtyPanels().size() == 3);
  CHECK(pipeline.updates().size() == 3);
  bool found_aggregate = false;
  for (const auto& update : pipeline.updates()) {
    if (update.panel_id == "p1") {
      found_aggregate = update.aggregate.valid;
      checkNear(update.aggregate.value, 10);
    }
  }
  CHECK(found_aggregate);
  pipeline.clearUpdates();

  tick = pipeline.tick(now + 1000);
  CHECK(tick.http_requests == 0 && tick.dirty_panels == 0);
  transport.response.body = R"({"metrics":[
    {"metric":"cpu","timestamp_ms":1760000000000,"value":10},
    {"metric":"mem","timestamp_ms":1760000000000,"value":25}
  ]})";
  tick = pipeline.tick(now + 60000);
  CHECK(tick.http_requests == 1 && tick.deduplicated_requests == 1);
  const std::vector<std::string> dirty = pipeline.takeDirtyPanels();
  CHECK(dirty.size() == 1 && dirty[0] == "p2");
  CHECK(pipeline.updates().size() == 1);
  pipeline.clearUpdates();

  transport.response.status = 500;
  transport.response.error = "mock outage";
  tick = pipeline.tick(now + 120000);
  CHECK(tick.failed == 2);
  CHECK(pipeline.takeDirtyPanels().size() == 3);
  SourceStatus status;
  CHECK(pipeline.sourceStatus("s1", status));
  CHECK(status.offline && status.consecutive_failures == 1);
  CHECK(pipeline.sourceResult("s1") != nullptr);
  CHECK(pipeline.sourceResult("s1")->metrics.size() == 2);
  pipeline.clearUpdates();
  const size_t points = pipeline.history().pointCount();

  transport.response.status = 304;
  transport.response.not_modified = true;
  transport.response.error.clear();
  transport.response.body.clear();
  tick = pipeline.tick(now + 240000);
  CHECK(tick.succeeded == 2 && tick.failed == 0);
  CHECK(pipeline.history().pointCount() == points);
  CHECK(pipeline.sourceStatus("s1", status));
  CHECK(!status.offline && !status.stale && status.error.empty());
  CHECK(!pipeline.setPanelRefreshOverride("p1", 120000, error));
  CHECK(pipeline.setPanelRefreshOverride("p1", 300000, error));
}

void testStaticSources() {
  SourcePlan text_plan = plan("text", ResponseKind::Text);
  text_plan.source.kind = SourceKind::Static;
  text_plan.source.url.clear();
  text_plan.static_body = "hello ${name}";
  eego::dashboard::Dashboard model = dashboard({panel("text_panel", "text", "")});
  Variable variable;
  variable.name = "name";
  variable.kind = VariableKind::Constant;
  variable.default_value = "world";
  model.variables.push_back(variable);

  eego::dashboard::Source static_source;
  static_source.id = "samples";
  static_source.kind = SourceKind::Static;
  model.sources.push_back(static_source);
  model.pages[0].panels.push_back(panel("sample_panel", "samples", "fixed"));
  model.samples.push_back(sample("samples", "fixed", 1000, 7));

  MockTransport transport;
  DataPipeline pipeline(transport);
  std::string error;
  CHECK(pipeline.configure(model, {text_plan}, nullptr, error));
  CHECK(pipeline.tick(2000).succeeded == 2);
  CHECK(transport.calls == 0);
  CHECK(pipeline.sourceResult("text")->has_text);
  CHECK(pipeline.sourceResult("text")->text.text == "hello world");
  CHECK(pipeline.sourceResult("samples")->metrics.size() == 1);
  checkNear(pipeline.sourceResult("samples")->metrics[0].value, 7);
}

void testOfflineCacheRestoreAndStale() {
  TempDirectory temporary;
  SdCache cache(temporary.path());
  SourceResult cached;
  cached.source_id = "cached";
  cached.fetched_at_ms = 1760000000000LL;
  cached.metrics.push_back(sample("cached", "cpu", cached.fetched_at_ms, 4));
  std::string error;
  CHECK(cache.writeSnapshot(cached, error));
  CHECK(cache.appendHistory("cached", {{cached.metrics[0], 0}}, error));

  SourcePlan source_plan = plan("cached");
  source_plan.source.refresh_ms = 300000;
  source_plan.stale_after_ms = 60000;
  MockTransport transport;
  transport.response.status = 500;
  transport.response.error = "offline";
  DataPipeline pipeline(transport);
  CHECK(pipeline.configure(dashboard({panel("p", "cached", "cpu", 60000)}), {source_plan},
                           &cache, error));
  CHECK(pipeline.history().pointCount() == 1);
  pipeline.tick(cached.fetched_at_ms + 1000);
  CHECK(pipeline.sourceResult("cached") != nullptr);
  SourceStatus status;
  CHECK(pipeline.sourceStatus("cached", status));
  CHECK(status.offline && !status.stale);
  CHECK(pipeline.takeDirtyPanels().size() == 1);
  pipeline.clearUpdates();

  pipeline.tick(cached.fetched_at_ms + 61000);
  CHECK(transport.calls == 1);
  CHECK(pipeline.sourceStatus("cached", status));
  CHECK(status.offline && status.stale);
  CHECK(pipeline.takeDirtyPanels().size() == 1);
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests = {
      {"json_path_and_request_plan", testJsonPathAndRequestPlan},
      {"generic_mappings_and_presets", testGenericMappingsAndPresets},
      {"standard_and_prometheus", testStandardAndPrometheus},
      {"text_and_image", testTextAndImage},
      {"windows_and_aggregation", testWindowsAndAggregation},
      {"history_bounds_and_reset_aggregation", testHistoryBoundsAndResetAggregation},
      {"cache_roundtrip_and_limits", testCacheRoundTripAndLimits},
      {"scheduler_dedup_fanout_failure", testSchedulerDedupFanoutAndFailure},
      {"static_sources", testStaticSources},
      {"offline_cache_restore_and_stale", testOfflineCacheRestoreAndStale},
  };
  size_t failures = 0;
  for (const auto& test : tests) {
    try {
      test.second();
      std::cout << "PASS " << test.first << "\n";
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL " << test.first << ": " << error.what() << "\n";
    }
  }
  std::cout << (tests.size() - failures) << "/" << tests.size() << " tests passed\n";
  return failures == 0 ? 0 : 1;
}
