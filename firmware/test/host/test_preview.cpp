// SPDX-License-Identifier: GPL-3.0-or-later
// Render preview (admin POST /dashboard/render-preview): preview data rules
// and the offscreen render -> 4-bpp BMP path with the real renderer.
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "admin/display_snapshot.h"
#include "dashboard_core/config.h"
#include "dashboard_ui/preview_data.h"
#include "dashboard_ui/preview_render.h"
#include "host_font.h"
#include "portrait_canvas.h"
#include "test_util.h"
#include "uc8279c_display.h"

namespace {

using namespace eego::dashboard;

Dashboard parseExample(const char* name, size_t* sample_count = nullptr) {
  hosttest::JsonValue root;
  std::string error;
  const std::string text = hosttest::slurp(hosttest::firmwareRoot() / "presets/examples" / name);
  REQUIRE_MSG(hosttest::parseJson(text, root, &error), error);
  const hosttest::JsonValue* dash = hosttest::member(root, "dashboard");
  REQUIRE(dash != nullptr);
  ParseResult parsed = dashboardFromJson(*dash);
  REQUIRE_MSG(parsed.ok(), name << ": " << (parsed.errors.empty() ? "" : parsed.errors[0].message));
  if (sample_count != nullptr) *sample_count = parsed.dashboard.samples.size();
  return parsed.dashboard;
}

Dashboard metricDraft() {
  Dashboard dash;
  Source source;
  source.id = "srv";
  dash.sources.push_back(source);
  Page page;
  page.id = "main";
  PanelSpec card;
  card.id = "cpu";
  card.kind = PanelKind::Card;
  card.source_id = "srv";
  card.metric = "cpu";
  card.style.unit = "%";
  card.grid = {0, 0, 12, 4};
  page.panels.push_back(card);
  PanelSpec chart = card;
  chart.id = "load";
  chart.kind = PanelKind::Chart;
  chart.metric = "load";
  chart.style.unit = "";
  chart.grid = {12, 0, 12, 4};
  page.panels.push_back(chart);
  dash.pages.push_back(page);
  dash.default_page_id = "main";
  return dash;
}

}  // namespace

TEST(preview, bundle_samples_are_kept) {
  Dashboard draft = metricDraft();
  MetricSample sample;
  sample.source_id = "srv";
  sample.metric = "cpu";
  sample.value = 42;
  draft.samples.push_back(sample);
  Dashboard live = metricDraft();
  live.samples.push_back(sample);
  live.samples.back().value = 7;
  CHECK_EQ(std::string(ui::previewDataName(ui::fillPreviewData(draft, &live, 0))),
           std::string("bundle"));
  REQUIRE(draft.samples.size() == 1);
  CHECK_EQ(draft.samples[0].value, 42.0);
}

TEST(preview, live_data_fills_sources_without_bundle_samples) {
  Dashboard draft = metricDraft();
  Dashboard live = metricDraft();
  MetricSample sample;
  sample.source_id = "srv";
  sample.metric = "cpu";
  sample.value = 7;
  live.samples.push_back(sample);
  sample.source_id = "other";  // not a source of the draft
  live.samples.push_back(sample);
  Event event;
  event.id = "e";
  event.source_id = "srv";
  live.events.push_back(event);
  CHECK_EQ(std::string(ui::previewDataName(ui::fillPreviewData(draft, &live, 0))),
           std::string("live"));
  CHECK_EQ(draft.samples.size(), size_t{1});
  CHECK_EQ(draft.events.size(), size_t{1});
}

TEST(preview, placeholders_are_deterministic_web_demo_series) {
  Dashboard a = metricDraft();
  Dashboard b = metricDraft();
  const int64_t now = 1700000000000LL;
  CHECK_EQ(std::string(ui::previewDataName(ui::fillPreviewData(a, nullptr, now))),
           std::string("placeholder"));
  ui::fillPreviewData(b, nullptr, now);
  REQUIRE(a.samples.size() == 48);  // 24 points per metric panel
  for (size_t i = 0; i < a.samples.size(); ++i) CHECK_EQ(a.samples[i].value, b.samples[i].value);
  // "%" panels use the 30..90 band, others 10..99 (web/src/model/series.ts).
  for (const MetricSample& s : a.samples) {
    if (s.metric == "cpu") CHECK(s.value >= 30 && s.value <= 90);
    if (s.metric == "load") CHECK(s.value >= 10 && s.value <= 99);
  }
  CHECK_EQ(a.samples[23].timestamp_ms, now / 300000 * 300000);
  // Same walk as web demoSeries(panel.id + metric) (reference values computed with node).
  CHECK_NEAR(a.samples[0].value, 57.3, 0.001);
  CHECK_NEAR(a.samples[1].value, 54.2, 0.001);
  CHECK_NEAR(a.samples[24].value, 63.5, 0.001);
  CHECK_NEAR(a.samples[26].value, 57.5, 0.001);
}

TEST(preview, renders_page_to_4bpp_bmp) {
  eego::render_host::useFallbackFont();
  Dashboard dash = parseExample("server-metrics.json");
  ui::fillPreviewData(dash, nullptr, 1700000000000LL);
  auto display = std::make_unique<eego::Uc8279cDisplay>();
  REQUIRE(display->beginOffscreen() == ESP_OK);
  eego::PortraitCanvas canvas(*display);
  ui::PreviewRenderOptions options;
  const ui::PreviewRenderResult result = ui::renderPreviewPage(canvas, dash, options);
  REQUIRE_MSG(result.ok, result.error);
  CHECK(!result.page_id.empty());
  CHECK(result.screen_count >= 1);

  std::vector<uint8_t> bmp(eego::admin::kPreviewFileBytes);
  eego::admin::writePreviewBmpHeader(bmp.data());
  CHECK_EQ(eego::admin::kPreviewStride, size_t{276});
  CHECK(bmp[0] == 'B' && bmp[1] == 'M');
  CHECK_EQ(bmp[28], uint8_t{4});
  uint8_t* rows = bmp.data() + eego::admin::kPreviewHeaderBytes;
  ui::packGrayLevels(*display, rows, eego::admin::kPreviewStride, false);
  size_t black = 0;
  size_t gray = 0;
  for (size_t y = 0; y < eego::admin::kSnapshotHeight; ++y) {
    for (size_t x = 0; x < eego::admin::kSnapshotWidth; ++x) {
      const uint8_t byte = rows[y * eego::admin::kPreviewStride + x / 2];
      const uint8_t level = (x & 1) ? (byte & 0x0f) : (byte >> 4);
      CHECK(level <= 3);
      CHECK_EQ(level, display->grayLevel(static_cast<int16_t>(x), static_cast<int16_t>(y)));
      if (level == 0) ++black;
      if (level == 1 || level == 2) ++gray;
    }
  }
  CHECK(black > 1000);  // frames, text, values
  {
    std::ofstream out(hosttest::fs::temp_directory_path() / "eego_render_preview.bmp",
                      std::ios::binary);
    out.write(reinterpret_cast<const char*>(bmp.data()), static_cast<std::streamsize>(bmp.size()));
  }
  // Fast view: only black/white.
  ui::packGrayLevels(*display, rows, eego::admin::kPreviewStride, true);
  for (size_t i = 0; i < eego::admin::kPreviewStride * eego::admin::kSnapshotHeight; ++i) {
    const uint8_t hi = rows[i] >> 4;
    const uint8_t lo = rows[i] & 0x0f;
    if ((hi != 0 && hi != 3) || (lo != 0 && lo != 3)) {
      CHECK_MSG(false, "non b/w nibble at " << i);
      break;
    }
  }
  (void)gray;
}

TEST(preview, rejects_unknown_page_and_screen) {
  eego::render_host::useFallbackFont();
  Dashboard dash = metricDraft();
  auto display = std::make_unique<eego::Uc8279cDisplay>();
  eego::PortraitCanvas canvas(*display);
  ui::PreviewRenderOptions options;
  options.page_id = "nope";
  ui::PreviewRenderResult result = ui::renderPreviewPage(canvas, dash, options);
  CHECK(!result.ok);
  CHECK(result.bad_request);
  options.page_id = "main";
  options.screen = 1;
  result = ui::renderPreviewPage(canvas, dash, options);
  CHECK(!result.ok);
  CHECK(result.bad_request);
  CHECK_EQ(result.screen_count, size_t{1});
  options.screen = 0;
  result = ui::renderPreviewPage(canvas, dash, options);
  CHECK(result.ok);
}
