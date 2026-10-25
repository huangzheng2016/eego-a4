// SPDX-License-Identifier: GPL-3.0-or-later
#include "dashboard_runtime.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

#include "admin/dashboard_config.h"
#include "alert_core/snapshot.h"
#include "alert_core/types.h"
#include "dashboard_core/config.h"
#include "dashboard_core/expression.h"
#include "dashboard_core/json.h"
#include "dashboard_ui/layout.h"
#include "dashboard_ui/image_result.h"
#include "dashboard_ui/panel_hit.h"
#include "dashboard_ui/preview_data.h"
#include "dashboard_ui/preview_render.h"
#include "dashboard_ui/renderer.h"
#include "dashboard_ui/text.h"
#include "data_pipeline/data_pipeline.h"
#include "dashboard_diag.h"
#include "dashboard_events.h"
#include "dashboard_power.h"
#include "dashboard_publication.h"
#include "eego_hal.h"
#include "eego_ui.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "net_stack/net_stack.h"
#include "nvs.h"
#include "time_source.h"

namespace panel {
namespace {

namespace dash = eego::dashboard;
namespace dp = eego::dashboard::pipeline;
namespace ui = eego::dashboard::ui;

constexpr char kTag[] = "dashboard";
constexpr size_t kMaxBundleBytes = 384 * 1024;
constexpr size_t kMaxPrefsBytes = 8192;
constexpr size_t kMaxSamples = 8192;
constexpr size_t kMaxEvents = 1024;
constexpr int64_t kRenderThrottleMs = 250;
constexpr int64_t kStatusPeriodMs = 30000;       // 时钟来源（SNTP/RTC）查询周期
constexpr int64_t kStatusCheckMs = 1000;         // 状态行文本变化检查周期（仅文本变化才重绘）
constexpr int64_t kStoreCheckMs = 2000;          // DashboardStore 已提交修订号检查周期
constexpr int64_t kCarouselPeriodMs = 15000;

int64_t monotonicMs() { return esp_timer_get_time() / 1000; }
int64_t pipelineMs() { return static_cast<int64_t>(TimeSource::now()) * 1000; }

bool readBoundedFile(const std::string& path, size_t maximum, std::string& out,
                     std::string& error) {
  out.clear();
  FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    error = "configuration file not found";
    return false;
  }
  char buffer[4096];
  bool ok = true;
  while (true) {
    const size_t count = std::fread(buffer, 1, sizeof(buffer), file);
    if (count > 0) {
      if (out.size() + count > maximum) {
        error = "configuration bundle exceeds byte budget";
        ok = false;
        break;
      }
      out.append(buffer, count);
    }
    if (count < sizeof(buffer)) {
      if (std::ferror(file)) {
        error = "configuration read failed";
        ok = false;
      }
      break;
    }
  }
  std::fclose(file);
  if (!ok) out.clear();
  return ok;
}

std::string firstParseError(const dash::ParseResult& parsed) {
  if (parsed.errors.empty()) return "invalid dashboard";
  return parsed.errors.front().field + ": " + parsed.errors.front().message;
}

bool knownBundleFields(const dash::JsonValue& root, std::string& error) {
  static const std::set<std::string> known{"schema", "schema_version", "revision",
                                           "dashboard", "source_plans"};
  for (const auto& member : root.objectItems()) {
    if (known.count(member.first) == 0) {
      error = "unknown dashboard bundle field: " + member.first;
      return false;
    }
  }
  return true;
}

bool parseConfig(const std::string& text, dash::Dashboard& dashboard,
                 std::vector<dp::SourcePlan>& plans, uint32_t& revision,
                 std::string& error) {
  plans.clear();
  revision = 0;
  dash::JsonValue root;
  dash::JsonParseError json_error;
  if (!dash::jsonParse(text, root, json_error, 17)) {
    error = "dashboard JSON at byte " + std::to_string(json_error.offset) + ": " +
            json_error.message;
    return false;
  }
  const dash::JsonValue* schema = root.find("schema");
  const bool bundle = schema != nullptr && schema->type() == dash::JsonValue::Type::String &&
                      schema->stringValue() == "eego.dashboard-bundle/1";
  if (!bundle) {
    const dash::ParseResult parsed = dash::dashboardFromJson(root);
    if (!parsed.ok()) {
      error = firstParseError(parsed);
      return false;
    }
    dashboard = parsed.dashboard;
    return true;
  }

  if (!knownBundleFields(root, error)) return false;
  const dash::JsonValue* version = root.find("schema_version");
  const dash::JsonValue* rev = root.find("revision");
  const dash::JsonValue* model = root.find("dashboard");
  if (version == nullptr || version->type() != dash::JsonValue::Type::Int ||
      version->intValue() != 1 || rev == nullptr ||
      rev->type() != dash::JsonValue::Type::Int || rev->intValue() < 0 ||
      rev->intValue() > std::numeric_limits<uint32_t>::max() || model == nullptr) {
    error = "invalid dashboard bundle version, revision, or dashboard member";
    return false;
  }
  const dash::ParseResult parsed = dash::dashboardFromJson(*model);
  if (!parsed.ok()) {
    error = firstParseError(parsed);
    return false;
  }
  dashboard = parsed.dashboard;
  revision = static_cast<uint32_t>(rev->intValue());

  std::set<std::string> plan_ids;
  if (const dash::JsonValue* source_plans = root.find("source_plans")) {
    if (source_plans->type() != dash::JsonValue::Type::Array ||
        source_plans->arrayItems().size() > 12) {
      error = "source_plans must be an array of at most 12 entries";
      return false;
    }
    for (const dash::JsonValue& value : source_plans->arrayItems()) {
      dp::SourcePlan plan;
      if (!dp::sourcePlanFromJson(value, plan, error)) return false;
      bool known_source = false;
      for (const dash::Source& source : dashboard.sources) {
        if (source.id == plan.source.id) known_source = true;
      }
      if (!known_source || !plan_ids.insert(plan.source.id).second) {
        error = "source plan has an unknown or duplicate source ID";
        return false;
      }
      plans.push_back(std::move(plan));
    }
  }
  return true;
}

bool validPanelId(std::string_view id) {
  if (id.empty() || id.size() > 64) return false;
  for (const char c : id) {
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-')) {
      return false;
    }
  }
  return true;
}

bool validInterval(uint32_t value) {
  return value == 0 || value == 60000 || value == 300000 || value == 900000 ||
         value == 1800000;
}

void put16(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value));
  out.push_back(static_cast<uint8_t>(value >> 8));
}

void put32(std::vector<uint8_t>& out, uint32_t value) {
  for (size_t i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

uint16_t get16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) | static_cast<uint16_t>(data[1]) << 8;
}

uint32_t get32(const uint8_t* data) {
  uint32_t value = 0;
  for (size_t i = 0; i < 4; ++i) value |= static_cast<uint32_t>(data[i]) << (i * 8);
  return value;
}

std::string sampleKey(const dash::MetricSample& sample) {
  char number[64];
  std::snprintf(number, sizeof(number), "%lld/%.17g",
                static_cast<long long>(sample.timestamp_ms), sample.value);
  std::string key = sample.source_id + "\n" + sample.metric + "\n" + number + "\n" + sample.unit;
  for (const dash::KeyValue& label : sample.labels) {
    key += "\n" + label.key + "=" + label.value;
  }
  return key;
}

std::string eventKey(const dash::Event& event) {
  return dashboardEventIdentity(event) + "\n" + std::to_string(event.timestamp_ms) + "\n" +
         std::to_string(static_cast<unsigned>(event.state));
}

}  // namespace

struct DashboardRuntime::Impl {
  struct Location {
    std::string page_id;
    size_t page_index = 0;
    std::string panel_id;
    std::string event_id;
  };

  using RuleState = DashboardDerivedRuleState;

  struct RuntimeImageProvider final : public ui::ImageProvider {
    explicit RuntimeImageProvider(Impl* impl) : owner(impl) {}
    Impl* owner;
    bool load(const dash::Asset& asset, std::string_view resolved_uri,
              ui::ImageBuffer& output) override {
      return owner->loadImage(asset, resolved_uri, output);
    }
  };

  Deps deps;
  bool ready = false;
  bool cache_ready = false;
  std::string config_error;
  uint32_t revision = 0;
  dash::Dashboard config;
  dash::Dashboard view;
  std::vector<dp::SourcePlan> plans;
  std::unique_ptr<dp::NetStackTransport> transport;
  std::unique_ptr<dp::DataPipeline> pipeline;
  std::unique_ptr<dp::SdCache> cache;
  ui::DashboardRenderer renderer;
  RuntimeImageProvider image_provider{this};

  dash::VariableValues overrides;
  dash::VariableValues variables;
  std::map<std::string, uint32_t> refresh_overrides;
  std::map<std::string, std::string> chart_selection;
  std::map<std::string, RuleState> rule_states;
  DashboardEventTracker event_tracker;
  DashboardPublicationStore published_panels;
  std::vector<dash::MetricSample> raw_samples;
  std::map<std::string, dash::MetricSample> rule_samples;
  std::map<std::string, std::shared_ptr<const dp::SourceResult>> injected;
  std::map<std::string, std::shared_ptr<const dp::SourceResult>> source_pointers;
  std::map<std::string, dp::SourceStatus> source_statuses;

  std::string page_id;
  size_t page_index = 0;
  size_t focus = 0;
  size_t action_focus = 0;
  std::string focused_event_id;
  // 面板焦点框只在用户显式选择面板（长按 ↑/↓、点按面板）后显示；翻屏清除。
  bool focus_visible = false;
  // 本次重建中因已确认而在视图里降为 Cleared 的事件（详情页显示「已确认」）。
  std::set<std::string> acknowledged_events;
  // 告警详情请求（PanelApp::service 取走）。
  bool detail_requested = false;
  std::string detail_panel;
  std::string detail_identity;
  uint32_t global_refresh_ms = 0;
  ui::PageLayout layout;
  std::vector<Location> navigation;
  uint64_t page_epoch = 1;

  bool dirty = true;
  bool force_full = true;
  bool status_dirty = true;
  bool force_tick = true;
  bool mains = true;
  bool battery_cycle_active = false;
  bool battery_cycle_cancelled = false;
  int64_t battery_cycle_deadline_ms = 0;
  int64_t last_tick_ms = 0;
  int64_t next_tick_ms = 0;
  int64_t last_render_ms = -10000;
  int64_t last_status_ms = -100000;
  int64_t last_clock_query_ms = -100000;
  const char* clock_label = "NOCLK";
  std::string drawn_status_text;
  // web 端 ApplyDashboard / 删除 → DashboardStore 修订号变化 → reload()/卸载
  int64_t last_store_check_ms = -100000;
  uint64_t store_revision = 0;
  bool store_configured = false;
  bool config_changed = false;  // web 端应用/删除 dashboard（PanelApp 视为一次交互）
  bool store_seen = false;
  int64_t last_rule_eval_ms = -100000;
  int64_t last_carousel_ms = 0;
  uint64_t alert_sequence = 0;
  std::set<std::string> alert_core_links;
  dp::TickResult last_tick;
  ui::RenderResult last_render;
  std::string message;
  // 电池深睡帧的状态行（电量/更新于/最迟，或夜间暂停）；空 = 常规状态行。
  std::string battery_status;
  int64_t message_deadline_ms = 0;
  size_t render_count = 0;
  size_t skipped_render_count = 0;

  const dash::Page* page(const dash::Dashboard& model, std::string_view id) const {
    for (const dash::Page& candidate : model.pages) {
      if (candidate.visible && candidate.id == id) return &candidate;
    }
    return nullptr;
  }

  dash::Page* page(dash::Dashboard& model, std::string_view id) {
    for (dash::Page& candidate : model.pages) {
      if (candidate.visible && candidate.id == id) return &candidate;
    }
    return nullptr;
  }

  const dash::Page* currentPage() const { return page(view, page_id); }
  dash::Page* currentPage() { return page(view, page_id); }

  const dash::Page* defaultPage(const dash::Dashboard& model) const {
    const dash::Page* fallback = nullptr;
    for (const dash::Page& candidate : model.pages) {
      if (!candidate.visible) continue;
      if (fallback == nullptr) fallback = &candidate;
      if (candidate.id == model.default_page_id) return &candidate;
    }
    return fallback;
  }

  std::vector<const dash::Page*> visiblePages() const {
    std::vector<const dash::Page*> out;
    for (const dash::Page& candidate : view.pages) {
      if (candidate.visible) out.push_back(&candidate);
    }
    return out;
  }

  const ui::PanelLayout* focusedLayout() const {
    if (focus >= layout.panels.size()) return nullptr;
    return &layout.panels[focus];
  }

  const dash::PanelSpec* focusedPanel() const {
    const ui::PanelLayout* selected = focusedLayout();
    return selected == nullptr ? nullptr : selected->panel;
  }

  const dash::PanelSpec* findPanel(std::string_view id, const dash::Page** owner = nullptr,
                                   size_t* subpage = nullptr) const {
    for (const dash::Page& candidate : view.pages) {
      if (!candidate.visible) continue;
      for (const dash::PanelSpec& panel : candidate.panels) {
        if (panel.id != id) continue;
        if (owner != nullptr) *owner = &candidate;
        if (subpage != nullptr) *subpage = ui::panelScreenIndex(panel);
        return &panel;
      }
    }
    return nullptr;
  }

  // 面板布局区 = 普通 UI 安全区减去底部状态行（渲染与命中测试共用同一配置）。
  static ui::LayoutConfig layoutConfig() {
    ui::LayoutConfig config;
    config.content = ui::Rect{ui::kDashboardContent.x, ui::kDashboardContent.y,
                              ui::kDashboardContent.w,
                              static_cast<int16_t>(kStatusLineY - ui::kDashboardContent.y)};
    return config;
  }

  void rebuildLayout() {
    const dash::Page* current = currentPage();
    if (current == nullptr) {
      layout = ui::PageLayout{};
      focus = 0;
      return;
    }
    const ui::LayoutConfig config = layoutConfig();
    layout = ui::buildPageLayout(*current, page_index, config);
    if (layout.page_count == 0) page_index = 0;
    if (page_index >= layout.page_count) {
      page_index = layout.page_count - 1;
      layout = ui::buildPageLayout(*current, page_index, config);
    }
    if (focus >= layout.panels.size()) focus = layout.panels.empty() ? 0 : layout.panels.size() - 1;
    if (const dash::PanelSpec* selected = focusedPanel();
        selected == nullptr || action_focus >= selected->actions.size()) {
      action_focus = 0;
    }
  }

  void showMessage(std::string text, uint32_t duration_ms = 3000) {
    message = ui::sanitizeText(text, false, 100);
    message_deadline_ms = monotonicMs() + duration_ms;
    status_dirty = true;
    dirty = true;
  }

  void invalidateFocus(std::string_view old_id = {}) {
    if (!old_id.empty()) renderer.invalidatePanel(old_id);
    if (const dash::PanelSpec* selected = focusedPanel()) renderer.invalidatePanel(selected->id);
    status_dirty = true;
    dirty = true;
  }

  void loadPrefs() {
    refresh_overrides.clear();
    nvs_handle_t handle;
    if (nvs_open("dashboard", NVS_READONLY, &handle) != ESP_OK) return;
    size_t size = 0;
    if (nvs_get_blob(handle, "panels", nullptr, &size) == ESP_OK && size >= 6 &&
        size <= kMaxPrefsBytes) {
      std::vector<uint8_t> data(size);
      if (nvs_get_blob(handle, "panels", data.data(), &size) == ESP_OK &&
          std::memcmp(data.data(), "DPR1", 4) == 0) {
        const uint16_t count = get16(data.data() + 4);
        size_t offset = 6;
        bool valid = count <= 72;
        std::map<std::string, uint32_t> loaded;
        for (size_t i = 0; valid && i < count; ++i) {
          if (offset + 6 > data.size()) {
            valid = false;
            break;
          }
          const uint16_t length = get16(data.data() + offset);
          offset += 2;
          const uint32_t interval = get32(data.data() + offset);
          offset += 4;
          if (length == 0 || length > 64 || offset + length > data.size() ||
              !validInterval(interval)) {
            valid = false;
            break;
          }
          const std::string id(reinterpret_cast<const char*>(data.data() + offset), length);
          offset += length;
          if (!validPanelId(id) || loaded.count(id) != 0) {
            valid = false;
            break;
          }
          if (interval != 0) loaded.emplace(id, interval);
        }
        if (valid && offset == data.size()) refresh_overrides = std::move(loaded);
      }
    }
    nvs_close(handle);
  }

  bool savePrefs() {
    std::vector<uint8_t> data{'D', 'P', 'R', '1', 0, 0};
    const uint16_t count = static_cast<uint16_t>(std::min<size_t>(refresh_overrides.size(), 72));
    data[4] = static_cast<uint8_t>(count);
    data[5] = static_cast<uint8_t>(count >> 8);
    size_t written = 0;
    for (const auto& item : refresh_overrides) {
      if (written++ >= count) break;
      put16(data, static_cast<uint16_t>(item.first.size()));
      put32(data, item.second);
      data.insert(data.end(), item.first.begin(), item.first.end());
    }
    if (data.size() > kMaxPrefsBytes) return false;
    nvs_handle_t handle;
    if (nvs_open("dashboard", NVS_READWRITE, &handle) != ESP_OK) return false;
    const esp_err_t set_err = nvs_set_blob(handle, "panels", data.data(), data.size());
    const esp_err_t commit_err = set_err == ESP_OK ? nvs_commit(handle) : set_err;
    nvs_close(handle);
    return commit_err == ESP_OK;
  }

  // 面板刷新周期覆盖：用户在面板上单独设定的优先，其次全局覆盖
  // （DeviceSettings.data_refresh_s），都没有则按配置。未调度的面板忽略。
  void applyRefreshOverrides(dp::DataPipeline& target, const dash::Dashboard& model) {
    for (const dash::Page& current : model.pages) {
      for (const dash::PanelSpec& panel : current.panels) {
        if (panel.source_id.empty()) continue;
        const auto custom = refresh_overrides.find(panel.id);
        const uint32_t interval =
            custom != refresh_overrides.end() ? custom->second : global_refresh_ms;
        std::string ignored;
        target.setPanelRefreshOverride(panel.id, interval, ignored);
      }
    }
  }

  void setGlobalRefresh(uint32_t refresh_ms) {
    if (refresh_ms == global_refresh_ms) return;
    global_refresh_ms = refresh_ms;
    if (!ready || pipeline == nullptr) return;
    applyRefreshOverrides(*pipeline, config);
    force_tick = true;
  }

  void ensureCache() {
    if (cache != nullptr || !eego::SdCard::instance().ready()) return;
    std::string relative = deps.cache_dir;
    if (relative.rfind("/sdcard/", 0) == 0) {
      relative.erase(0, 8);
    } else {
      while (!relative.empty() && relative.front() == '/') relative.erase(relative.begin());
    }
    cache_ready = eego::SdCard::instance().mkdir(relative.c_str());
    if (cache_ready) cache = std::make_unique<dp::SdCache>(deps.cache_dir);
  }

  bool begin(const Deps& initial) {
    deps = initial;
    loadPrefs();
    if (deps.net != nullptr) transport = std::make_unique<dp::NetStackTransport>(*deps.net);
    renderer.setImageProvider(&image_provider);
    return reload();
  }

  bool reload() {
    if (deps.net == nullptr || transport == nullptr) {
      config_error = "net_task transport is not wired";
      return false;
    }
    dash::Dashboard next_config;
    std::vector<dp::SourcePlan> next_plans;
    uint32_t next_revision = 0;
    std::string text;
    std::string error;
    if (!readBoundedFile(deps.config_path, kMaxBundleBytes, text, error) ||
        !parseConfig(text, next_config, next_plans, next_revision, error)) {
      config_error = ui::sanitizeText(error, false, 160);
      if (!ready) ESP_LOGW(kTag, "dashboard unavailable: %s", config_error.c_str());
      return false;
    }
    if (defaultPage(next_config) == nullptr) {
      config_error = "dashboard has no visible page";
      return false;
    }

    dash::VariableValues next_overrides;
    for (const auto& value : overrides) {
      for (const dash::Variable& variable : next_config.variables) {
        if (variable.name == value.first) next_overrides.push_back(value);
      }
    }
    dash::VariableValues next_variables;
    if (!dash::defaultVariableValues(next_config.variables, next_overrides, next_variables, error)) {
      config_error = ui::sanitizeText(error, false, 160);
      return false;
    }
    ensureCache();
    dp::PipelineConfig pipeline_config;
    pipeline_config.default_panel_refresh_ms = next_config.refresh_ms;
    pipeline_config.variable_overrides = next_overrides;
    pipeline_config.token_resolver = dp::makeNetStackTokenResolver();
    auto candidate = std::make_unique<dp::DataPipeline>(*transport, pipeline_config);
    if (!candidate->configure(next_config, next_plans, cache.get(), error)) {
      config_error = ui::sanitizeText(error, false, 160);
      return false;
    }
    const std::set<std::string> linked_dependencies = dp::linkedPanelDependencies(next_config);
    for (const dash::Page& current : next_config.pages) {
      for (const dash::PanelSpec& panel : current.panels) {
        dp::Aggregation aggregation;
        if (!dp::panelRequiresScheduling(panel, linked_dependencies) ||
            !dashboardPanelRequiresPublication(panel) ||
            !dashboardAggregationForExpression(panel.value_expression, aggregation)) {
          continue;
        }
        dp::PanelBinding binding;
        binding.panel_id = panel.id;
        binding.source_id = panel.source_id;
        binding.metric = panel.metric;
        binding.window = dashboardWindowFromPanel(panel);
        binding.time_window = panel.time_window;
        binding.aggregation = aggregation;
        binding.has_aggregation = true;
        binding.has_custom_window = dashboardPanelHasCustomWindow(panel);
        if (!candidate->setPanelBinding(binding, error)) {
          config_error = ui::sanitizeText(error, false, 160);
          return false;
        }
      }
    }
    applyRefreshOverrides(*candidate, next_config);

    const bool suspend = dashboardReloadShouldSuspend(
        battery_cycle_cancelled, mains, battery_cycle_active, monotonicMs(),
        battery_cycle_deadline_ms);
    if (suspend) {
      battery_cycle_cancelled = true;
      if (pipeline != nullptr) pipeline->cancelAsync();
    }
    pipeline = std::move(candidate);
    if (suspend) pipeline->cancelAsync();
    config = std::move(next_config);
    plans = std::move(next_plans);
    overrides = std::move(next_overrides);
    variables = std::move(next_variables);
    revision = next_revision;
    ready = true;
    config_error.clear();
    source_pointers.clear();
    source_statuses.clear();
    injected.clear();
    published_panels.clear();
    raw_samples.clear();
    rule_samples.clear();
    banner_signature.clear();
    alert_core_links.clear();
    rule_states.clear();
    event_tracker.reset();
    navigation.clear();
    ++page_epoch;
    focused_event_id.clear();
    focus_visible = false;
    detail_requested = false;
    chart_selection.clear();
    const dash::Page* initial = page(config, page_id);
    if (initial == nullptr) initial = defaultPage(config);
    page_id = initial->id;
    page_index = 0;
    focus = 0;
    action_focus = 0;
    renderer.reset();
    rebuildData();
    rebuildLayout();
    force_tick = true;
    next_tick_ms = 0;
    last_status_ms = -100000;
    dirty = true;
    force_full = true;
    status_dirty = true;
    ESP_LOGI(kTag, "dashboard loaded: id=%s revision=%u pages=%u sources=%u", config.id.c_str(),
             static_cast<unsigned>(revision), static_cast<unsigned>(config.pages.size()),
             static_cast<unsigned>(config.sources.size()));
    return true;
  }

  void setPage(const std::string& id, size_t subpage, bool remember) {
    const dash::Page* target = page(view, id);
    if (target == nullptr) return;
    if (remember && currentPage() != nullptr) {
      if (navigation.size() >= 12) navigation.erase(navigation.begin());
      const dash::PanelSpec* selected = focusedPanel();
      navigation.push_back({page_id, page_index, selected == nullptr ? "" : selected->id,
                            focused_event_id});
    }
    page_id = id;
    page_index = subpage;
    focus = 0;
    action_focus = 0;
    focused_event_id.clear();
    focus_visible = false;
    ++page_epoch;
    rebuildLayout();
    dirty = true;
    force_full = true;
    status_dirty = true;
  }

  bool gotoPage(const std::string& id, bool remember = true) {
    if (page(view, id) == nullptr) return false;
    setPage(id, 0, remember);
    return true;
  }

  bool focusPanel(const std::string& id, bool remember = true) {
    const dash::Page* owner = nullptr;
    size_t subpage = 0;
    const dash::PanelSpec* target = findPanel(id, &owner, &subpage);
    if (target == nullptr || !target->visible || owner == nullptr) return false;
    setPage(owner->id, subpage, remember);
    for (size_t i = 0; i < layout.panels.size(); ++i) {
      if (layout.panels[i].panel != nullptr && layout.panels[i].panel->id == id) {
        focus = i;
        break;
      }
    }
    focus_visible = true;
    invalidateFocus();
    return true;
  }

  bool back() {
    if (navigation.empty()) return false;
    const Location previous = navigation.back();
    navigation.pop_back();
    if (page(view, previous.page_id) == nullptr) return false;
    setPage(previous.page_id, previous.page_index, false);
    if (!previous.panel_id.empty()) {
      for (size_t i = 0; i < layout.panels.size(); ++i) {
        if (layout.panels[i].panel != nullptr && layout.panels[i].panel->id == previous.panel_id) {
          focus = i;
          break;
        }
      }
    }
    focused_event_id = previous.event_id;
    invalidateFocus();
    return true;
  }

  void movePage(int delta) {
    const std::vector<const dash::Page*> pages = visiblePages();
    if (pages.empty()) return;
    size_t index = 0;
    for (size_t i = 0; i < pages.size(); ++i) {
      if (pages[i]->id == page_id) index = i;
    }
    int64_t next = (static_cast<int64_t>(index) + delta) % static_cast<int64_t>(pages.size());
    if (next < 0) next += static_cast<int64_t>(pages.size());
    setPage(pages[static_cast<size_t>(next)]->id, 0, false);
  }

  // 上/下一屏：子屏优先，跨页循环（向前跨页落在上一页的最后一个子屏）。
  void moveScreen(int delta) {
    const std::vector<const dash::Page*> pages = visiblePages();
    if (pages.empty() || delta == 0) return;
    int remaining = delta;
    while (remaining != 0) {
      const int step = remaining > 0 ? 1 : -1;
      remaining -= step;
      if (step > 0 && page_index + 1 < layout.page_count) {
        setPage(page_id, page_index + 1, false);
        continue;
      }
      if (step < 0 && page_index > 0) {
        setPage(page_id, page_index - 1, false);
        continue;
      }
      size_t index = 0;
      for (size_t i = 0; i < pages.size(); ++i) {
        if (pages[i]->id == page_id) index = i;
      }
      const size_t next = (index + pages.size() + static_cast<size_t>(step)) % pages.size();
      setPage(pages[next]->id, 0, false);
      if (step < 0 && layout.page_count > 1) setPage(page_id, layout.page_count - 1, false);
    }
  }

  void home() {
    const dash::Page* initial = defaultPage(view);
    if (initial == nullptr) return;
    navigation.clear();
    setPage(initial->id, 0, false);
  }

  bool atHome() const {
    const dash::Page* initial = defaultPage(view);
    return initial != nullptr && initial->id == page_id && page_index == 0;
  }

  void moveFocus(int delta) {
    if (delta == 0) return;
    if (!focus_visible && !layout.panels.empty()) {
      // 第一次长按只显示焦点框（落在当前面板），不跳过它。
      focus_visible = true;
      invalidateFocus();
      return;
    }
    const std::string old = focusedPanel() == nullptr ? "" : focusedPanel()->id;
    int remaining = delta;
    while (remaining != 0) {
      const int step = remaining > 0 ? 1 : -1;
      if (layout.panels.empty()) {
        movePage(step);
        remaining -= step;
        continue;
      }
      const int64_t next = static_cast<int64_t>(focus) + step;
      if (next < 0) {
        if (page_index > 0) {
          --page_index;
          rebuildLayout();
          focus = layout.panels.empty() ? 0 : layout.panels.size() - 1;
          force_full = true;
        } else {
          movePage(-1);
        }
      } else if (next >= static_cast<int64_t>(layout.panels.size())) {
        if (page_index + 1 < layout.page_count) {
          ++page_index;
          rebuildLayout();
          focus = 0;
          force_full = true;
        } else {
          movePage(1);
        }
      } else {
        focus = static_cast<size_t>(next);
      }
      remaining -= step;
    }
    focused_event_id.clear();
    action_focus = 0;
    focus_visible = true;
    invalidateFocus(old);
  }

  uint32_t effectiveInterval(const dash::PanelSpec& panel) const {
    const auto override_value = refresh_overrides.find(panel.id);
    if (override_value != refresh_overrides.end()) return override_value->second;
    return panel.refresh_ms == 0 ? config.refresh_ms : panel.refresh_ms;
  }

  void cycleInterval(int delta) {
    dash::PanelSpec* selected = nullptr;
    if (dash::Page* current = currentPage()) {
      for (dash::PanelSpec& panel : current->panels) {
        if (focusedPanel() != nullptr && panel.id == focusedPanel()->id) selected = &panel;
      }
    }
    if (selected == nullptr || selected->source_id.empty()) return;
    static const uint32_t intervals[] = {0, 60000, 300000, 900000, 1800000};
    const uint32_t current = refresh_overrides.count(selected->id) == 0
                                 ? 0
                                 : refresh_overrides[selected->id];
    size_t index = 0;
    for (size_t i = 0; i < 5; ++i) {
      if (intervals[i] == current) index = i;
    }
    index = (index + 5 + static_cast<size_t>(delta)) % 5;
    const uint32_t next = intervals[index];
    std::string error;
    if (!pipeline->setPanelRefreshOverride(selected->id, next, error)) {
      showMessage("Refresh override failed");
      return;
    }
    if (next == 0) {
      refresh_overrides.erase(selected->id);
    } else {
      refresh_overrides[selected->id] = next;
    }
    const bool saved = savePrefs();
    showMessage(selected->id + " refresh " +
                (next == 0 ? std::string("config") : std::to_string(next / 60000) + "m") +
                (saved ? "" : " (NVS save failed)"));
    force_tick = true;
    status_dirty = true;
  }

  std::string linkedForRecord(const alert_core::AlertRecord& record) const {
    if (record.fields.type() == alert_core::JsonValue::Type::Object) {
      for (const char* key : {"linked_panel_id", "dashboard_panel", "panel_id"}) {
        if (const alert_core::JsonValue* value = record.fields.find(key);
            value != nullptr && value->type() == alert_core::JsonValue::Type::String &&
            findPanel(value->stringValue()) != nullptr) {
          return value->stringValue();
        }
      }
    }
    for (const dash::AlertRule& rule : config.alert_rules) {
      if (rule.id == record.type && !rule.style.linked_panel_id.empty() &&
          findPanel(rule.style.linked_panel_id) != nullptr) {
        return rule.style.linked_panel_id;
      }
    }
    return {};
  }

  std::string linkedForEvent(const dash::Event& event) const {
    if (!event.linked_panel_id.empty() && findPanel(event.linked_panel_id) != nullptr)
      return event.linked_panel_id;
    for (const dash::AlertRule& rule : config.alert_rules) {
      if (rule.id == event.rule_id && !rule.style.linked_panel_id.empty() &&
          findPanel(rule.style.linked_panel_id) != nullptr) {
        return rule.style.linked_panel_id;
      }
    }
    return {};
  }

  void appendSample(const dash::MetricSample& sample, std::set<std::string>& seen) {
    if (view.samples.size() >= kMaxSamples || !seen.insert(sampleKey(sample)).second) return;
    view.samples.push_back(sample);
  }

  void appendEvent(const dash::Event& event, std::set<std::string>& seen) {
    if (view.events.size() >= kMaxEvents || !seen.insert(eventKey(event)).second) return;
    const uint64_t generation = event_tracker.observe(event);
    view.events.push_back(event);
    if (event_tracker.suppressed(event, generation)) {
      view.events.back().state = dash::EventState::Cleared;
      acknowledged_events.insert(dashboardEventIdentity(event));
    }
  }

  bool usesSeries(const dash::PanelSpec& panel) const {
    return dashboardPanelRequiresPublication(panel);
  }

  std::shared_ptr<const dp::SourceResult> currentResult(const std::string& source_id) const {
    const auto local = injected.find(source_id);
    return local != injected.end() ? local->second : pipeline->sourceResult(source_id);
  }

  bool loadImage(const dash::Asset& asset, std::string_view resolved_uri,
                 ui::ImageBuffer& output) {
    output.size = 0;
    if (output.data == nullptr || output.capacity == 0 ||
        (resolved_uri.substr(0, 7) != "http://" && resolved_uri.substr(0, 8) != "https://")) {
      return false;
    }
    const dash::Source* source =
        ui::dashboardImageSourceForUri(asset, resolved_uri, variables, config.sources);
    const std::string source_id = source == nullptr ? "" : source->id;
    if (source_id.empty()) return false;
    const std::shared_ptr<const dp::SourceResult> result = currentResult(source_id);
    if (result != nullptr && result->has_image) {
      if (!ui::copySourceImage(*result, output.data, output.capacity, output.size)) return false;
      dp::SourceStatus status;
      if (pipeline->sourceStatus(source_id, status) && (status.offline || status.stale)) {
        pipeline->scheduleSourceRefresh(source_id);
      }
      return true;
    }
    pipeline->scheduleSourceRefresh(source_id);
    return false;
  }

  void invalidateImageSource(const std::string& source_id,
                             const std::shared_ptr<const dp::SourceResult>& previous,
                             const std::shared_ptr<const dp::SourceResult>& current) {
    const bool previous_image = previous != nullptr && previous->has_image;
    const bool current_image = current != nullptr && current->has_image;
    if (previous_image == current_image) {
      if (!current_image) return;
      if (previous->image.media_type == current->image.media_type &&
          previous->image.bytes == current->image.bytes) {
        return;
      }
    }
    for (const dash::Asset& asset : config.assets) {
      if (asset.id == source_id) {
        renderer.invalidateImage(asset.id);
        continue;
      }
      for (const dash::Source& source : config.sources) {
        if (source.id == source_id &&
            ui::dashboardAssetMatchesSource(asset, source, variables)) {
          renderer.invalidateImage(asset.id);
        }
      }
    }
    for (const dash::Page& current_page : view.pages) {
      for (const dash::PanelSpec& panel : current_page.panels) {
        if (panel.kind != dash::PanelKind::Image) continue;
        const ui::ImageDependency dependency = ui::resolveImageDependency(view, panel, variables);
        if (dependency.asset == nullptr) continue;
        for (const dash::Source& source : config.sources) {
          if (source.id == source_id &&
              ui::dashboardSourceMatchesUri(source, dependency.uri, variables)) {
            renderer.invalidateImage(dependency.asset->id);
          }
        }
      }
    }
  }

  bool clearInjectedSource(const std::string& source_id) {
    if (injected.erase(source_id) == 0) return false;
    for (const dash::Page& current : view.pages) {
      for (const dash::PanelSpec& panel : current.panels) {
        if (panel.source_id == source_id) published_panels.erase(panel.id);
      }
    }
    return true;
  }

  bool reconcileInjections(std::set<std::string>& invalidated_links) {
    bool changed = false;
    for (const dash::Source& source : config.sources) {
      const auto known = source_pointers.find(source.id);
      const auto previous = known == source_pointers.end()
                                ? std::shared_ptr<const dp::SourceResult>()
                                : known->second;
      const std::shared_ptr<const dp::SourceResult> current = pipeline->sourceResult(source.id);
      if (!dashboardInjectionOutdated(previous, current)) continue;
      const auto local = injected.find(source.id);
      if (local != injected.end()) {
        invalidateImageSource(source.id, local->second, current);
        for (const dash::Event& event : local->second->events) {
          const std::string linked = linkedForEvent(event);
          if (!linked.empty()) invalidated_links.insert(linked);
        }
      }
      if (current != nullptr) {
        for (const dash::Event& event : current->events) {
          const std::string linked = linkedForEvent(event);
          if (!linked.empty()) invalidated_links.insert(linked);
        }
      }
      changed = clearInjectedSource(source.id) || changed;
    }
    return changed;
  }

  std::vector<dash::MetricSample> computePanelSamples(const dash::PanelSpec& panel) {
    std::vector<dash::MetricSample> out;
    if (panel.source_id.empty() || panel.metric.empty()) return out;
    dp::Aggregation aggregation;
    if (dashboardAggregationForExpression(panel.value_expression, aggregation)) {
      const int64_t now = pipelineMs();
      dp::AggregationResult aggregate;
      if (injected.count(panel.source_id) != 0) {
        const std::shared_ptr<const dp::SourceResult> effective = currentResult(panel.source_id);
        const dp::ResolvedWindow window =
            dashboardPanelHasCustomWindow(panel)
                ? dashboardResolvePanelWindow(panel, now)
                : dp::resolveWindow(dashboardWindowFromPanel(panel), now,
                                    pipeline->history().resetAt(panel.source_id, panel.metric), 0);
        aggregate = pipeline->history().aggregate(panel.source_id, panel.metric, {}, window,
                                                  aggregation, effective->metrics);
      } else if (dashboardPanelHasCustomWindow(panel)) {
        aggregate = pipeline->history().aggregate(panel.source_id, panel.metric, {},
                                                  dashboardResolvePanelWindow(panel, now),
                                                  aggregation);
      } else {
        aggregate = pipeline->history().aggregate(panel.source_id, panel.metric, {},
                                                  dashboardWindowFromPanel(panel), now,
                                                  aggregation, 0);
      }
      return dashboardAggregatePublication(panel, aggregate, now);
    }
    const dp::ResolvedWindow window = dashboardResolvePanelWindow(panel, pipelineMs());
    std::set<std::string> seen;
    auto append = [&](const dash::MetricSample& sample) {
      if (sample.source_id == panel.source_id && sample.metric == panel.metric &&
          dashboardSampleWithinWindow(sample, window) && seen.insert(sampleKey(sample)).second) {
        out.push_back(sample);
      }
    };
    for (const dash::MetricSample& sample : config.samples) append(sample);

    const std::vector<dash::MetricSample> history =
        pipeline->history().query(panel.source_id, panel.metric, {}, window);
    for (const dash::MetricSample& sample : history) append(sample);
    const std::shared_ptr<const dp::SourceResult> current = currentResult(panel.source_id);
    if (current != nullptr) {
      for (const dash::MetricSample& sample : current->metrics) append(sample);
    }
    std::stable_sort(out.begin(), out.end(), [](const dash::MetricSample& left,
                                                const dash::MetricSample& right) {
      return left.timestamp_ms < right.timestamp_ms;
    });
    for (dash::MetricSample& sample : out) {
      if (!dashboardEvaluateValueExpression(panel, sample)) {
        out.clear();
        return out;
      }
    }
    constexpr size_t kMaxPublishedSamples = 96;
    if (out.size() > kMaxPublishedSamples) {
      std::vector<dash::MetricSample> limited;
      limited.reserve(kMaxPublishedSamples);
      for (size_t i = 0; i < kMaxPublishedSamples; ++i)
        limited.push_back(out[i * (out.size() - 1) / (kMaxPublishedSamples - 1)]);
      out = std::move(limited);
    }
    return out;
  }

  void publishPanel(const dash::PanelSpec& panel, const dp::PanelUpdate* update = nullptr) {
    dp::Aggregation aggregation;
    if (update != nullptr && dashboardAggregationForExpression(panel.value_expression, aggregation)) {
      if (injected.count(panel.source_id) != 0) {
        published_panels.publish(panel.id, panel.metric, computePanelSamples(panel));
      } else {
        published_panels.publish(
            panel.id, panel.metric,
            dashboardAggregatePublication(panel, update->aggregate, pipelineMs()));
      }
      return;
    }
    published_panels.publish(panel.id, panel.metric, computePanelSamples(panel));
  }

  void publishRoute(const DashboardCarouselRoute& route, const dp::PanelUpdate* update) {
    const dash::PanelSpec* current = findPanel(route.owner_id);
    if (current == nullptr) return;
    dash::PanelSpec panel = *current;
    panel.source_id = route.source_id;
    panel.metric = route.metric;
    publishPanel(panel, update);
  }

  void publishPanelUpdate(const dp::PanelUpdate& update) {
    auto publish_for_owner = [&](const std::string& owner_id) {
      const std::string selected_id = dashboardResolveCarousel(chart_selection, owner_id);
      const dash::PanelSpec* selected = findPanel(selected_id);
      if (selected == nullptr) return;
      const DashboardCarouselRoute route = dashboardCarouselRoute(
          owner_id, selected_id, selected->source_id,
          published_panels.metric(selected_id, selected->metric), update.panel_id, update.source_id);
      if (route.allowed) publishRoute(route, &update);
    };
    publish_for_owner(update.panel_id);
    for (const auto& selection : chart_selection) {
      if (selection.first != update.panel_id) publish_for_owner(selection.first);
    }
  }

  void rebuildRuleSamples(
      const std::vector<std::shared_ptr<const dp::SourceResult>>& results, int64_t now) {
    rule_samples.clear();
    for (const dash::AlertRule& rule : config.alert_rules) {
      dash::MetricSample latest;
      bool found = false;
      auto consider = [&](const dash::MetricSample& sample) {
        if (sample.source_id != rule.source_id || sample.metric != rule.metric) return;
        if (!found || sample.timestamp_ms >= latest.timestamp_ms) {
          latest = sample;
          found = true;
        }
      };
      for (const dash::MetricSample& sample : config.samples) consider(sample);
      for (const auto& result : results) {
        for (const dash::MetricSample& sample : result->metrics) consider(sample);
      }
      const std::vector<dash::MetricSample> history =
          pipeline->history().query(rule.source_id, rule.metric, {}, {0, now});
      for (const dash::MetricSample& sample : history) consider(sample);
      if (found) rule_samples[rule.id] = std::move(latest);
    }
  }

  void rebuildData() {
    if (!ready || pipeline == nullptr) return;
    view = config;

    for (dash::Page& current : view.pages) {
      for (dash::PanelSpec& panel : current.panels) {
        if (panel.kind != dash::PanelKind::Chart) continue;
        const std::string selected_id = dashboardResolveCarousel(chart_selection, panel.id);
        if (selected_id == panel.id) continue;
        const dash::PanelSpec* candidate = nullptr;
        for (const dash::Page& source_page : config.pages) {
          for (const dash::PanelSpec& source_panel : source_page.panels) {
            if (source_panel.id == selected_id && source_panel.kind == dash::PanelKind::Chart) {
              candidate = &source_panel;
              break;
            }
          }
          if (candidate != nullptr) break;
        }
        if (candidate == nullptr) continue;
        panel.source_id = candidate->source_id;
        panel.metric = candidate->metric;
        panel.value_path = candidate->value_path;
        panel.value_expression = candidate->value_expression;
        panel.time_window = candidate->time_window;
        panel.thresholds = candidate->thresholds;
        panel.style = candidate->style;
        panel.chart = candidate->chart;
      }
    }

    std::vector<std::shared_ptr<const dp::SourceResult>> results;
    for (const dash::Source& source : config.sources) {
      const std::shared_ptr<const dp::SourceResult> current = pipeline->sourceResult(source.id);
      const auto known = source_pointers.find(source.id);
      if (known == source_pointers.end() || known->second != current) {
        clearInjectedSource(source.id);
        source_pointers[source.id] = current;
      }
      const std::shared_ptr<const dp::SourceResult> effective = currentResult(source.id);
      if (effective != nullptr) results.push_back(effective);
    }

    raw_samples.clear();
    std::set<std::string> raw_seen;
    auto append_raw = [&](const dash::MetricSample& sample) {
      if (raw_samples.size() < kMaxSamples && raw_seen.insert(sampleKey(sample)).second)
        raw_samples.push_back(sample);
    };
    for (const dash::MetricSample& sample : config.samples) append_raw(sample);
    for (const auto& result : results) {
      for (const dash::MetricSample& sample : result->metrics) append_raw(sample);
    }
    for (const dash::Variable& variable : config.variables) {
      if (variable.kind != dash::VariableKind::Query) continue;
      for (const dp::HistoryEntry& entry : pipeline->history().entries(variable.source_id))
        append_raw(entry.sample);
    }

    view.samples.clear();
    view.events.clear();
    acknowledged_events.clear();
    std::set<std::string> seen_samples;
    std::set<std::string> seen_events;
    for (dash::Page& current : view.pages) {
      for (dash::PanelSpec& panel : current.panels) {
        if (panel.kind == dash::PanelKind::Text && !panel.source_id.empty()) {
          const std::shared_ptr<const dp::SourceResult> result = currentResult(panel.source_id);
          if (result != nullptr && result->has_text)
            panel.text.content = ui::literalTextForTemplate(result->text.text);
        }
        if (!usesSeries(panel)) continue;
        if (!published_panels.contains(panel.id)) publishPanel(panel);
        const auto& published = published_panels.at(panel.id);
        const std::string alias = DashboardPublicationStore::alias(panel.id);
        for (dash::MetricSample sample : published.samples) {
          sample.metric = alias;
          appendSample(sample, seen_samples);
        }
        panel.metric = alias;
      }
    }

    for (const dash::Event& event : config.events) appendEvent(event, seen_events);
    for (const auto& result : results) {
      for (const dash::Event& event : result->events) appendEvent(event, seen_events);
    }
    if (deps.alerts != nullptr) {
      const auto snapshot = deps.alerts->acquire();
      if (snapshot != nullptr) {
        for (const alert_core::AlertRecord& record : snapshot->records) {
          if (!dashboardAlertRecordVisible(record)) continue;
          appendEvent(dashboardEventFromAlert(record, linkedForRecord(record)), seen_events);
        }
      }
    }

    const int64_t now = pipelineMs();
    rebuildRuleSamples(results, now);
    for (const auto& item : rule_states) {
      if (item.second.event_id.empty()) continue;
      const dash::AlertRule* rule = nullptr;
      for (const dash::AlertRule& candidate : config.alert_rules) {
        if ("rule_" + candidate.id == item.first) rule = &candidate;
      }
      if (rule == nullptr) continue;
      dash::Event event;
      event.id = item.second.event_id;
      event.rule_id = rule->id;
      event.source_id = rule->source_id;
      event.severity = rule->style.severity;
      event.state = item.second.active ? dash::EventState::Active : dash::EventState::Cleared;
      event.title = rule->title.empty() ? rule->id : rule->title;
      event.message = rule->message;
      event.linked_panel_id = rule->style.linked_panel_id;
      event.timestamp_ms = now;
      const auto sample = rule_samples.find(rule->id);
      if (sample != rule_samples.end()) event.timestamp_ms = sample->second.timestamp_ms;
      appendEvent(event, seen_events);
    }
    rebuildLayout();
    event_tracker.reconcile(view.events);
    updateBannerDirty();
  }

  bool eventMatches(const dash::PanelSpec& panel, const dash::Event& event) const {
    if (!panel.source_id.empty() && event.source_id != panel.source_id) return false;
    if (!panel.linked_panel_ids.empty() && event.linked_panel_id != panel.id) {
      bool linked = false;
      for (const std::string& id : panel.linked_panel_ids) linked = linked || id == event.linked_panel_id;
      if (!linked) return false;
    }
    if (!panel.metric.empty()) {
      bool metric_matches = false;
      for (const dash::AlertRule& rule : config.alert_rules) {
        if (rule.id == event.rule_id && rule.metric == panel.metric) metric_matches = true;
      }
      if (!metric_matches) return false;
    }
    return true;
  }

  // Display order of the panel's alert list (shared with the renderer).
  std::vector<const dash::Event*> eventsForPanel(const dash::PanelSpec& panel) const {
    return ui::alertPanelEvents(view, panel);
  }

  const dash::Event* findEvent(std::string_view id) const {
    if (id.empty()) return nullptr;
    const dash::Event* latest = nullptr;
    for (const dash::Event& event : view.events) {
      if (dashboardEventIdentity(event) == id &&
          (latest == nullptr || event.timestamp_ms >= latest->timestamp_ms)) {
        latest = &event;
      }
    }
    return latest;
  }

  void evaluateRules() {
    bool changed = false;
    const int64_t monotonic = monotonicMs();
    for (const dash::AlertRule& rule : config.alert_rules) {
      if (!rule.enabled) continue;
      const auto latest_value = rule_samples.find(rule.id);
      if (latest_value == rule_samples.end()) continue;
      const dash::MetricSample* latest = &latest_value->second;
      dash::EvalValues values{{"value", dash::EvalValue::makeNumber(latest->value)},
                              {"now", dash::EvalValue::makeNumber(
                                          static_cast<double>(latest->timestamp_ms))}};
      dash::EvalValue result;
      dash::ExpressionError error;
      if (!dash::evaluateExpression(rule.expression, values, result, error)) continue;
      const bool condition = result.type() == dash::EvalValue::Type::Bool
                                 ? result.boolean()
                                 : result.type() == dash::EvalValue::Type::Number &&
                                       result.number() != 0.0;
      RuleState& state = rule_states["rule_" + rule.id];
      if (condition) {
        if (state.true_since_ms < 0) state.true_since_ms = monotonic;
        if (!state.active && monotonic - state.true_since_ms >= rule.for_ms) {
          state.active = true;
          state.event_id = dashboardDerivedRuleEventId(rule.id);
          event_tracker.eraseAcknowledgement(rule.source_id, state.event_id);
          changed = true;
        }
      } else {
        state.true_since_ms = -1;
        if (state.active) {
          state.active = false;
          event_tracker.eraseAcknowledgement(rule.source_id, state.event_id);
          changed = true;
        }
      }
    }
    if (changed) {
      rebuildData();
      for (const dash::AlertRule& rule : config.alert_rules) {
        const auto state = rule_states.find("rule_" + rule.id);
        if (state != rule_states.end() && !rule.style.linked_panel_id.empty())
          renderer.invalidatePanel(rule.style.linked_panel_id);
        for (const dash::Page& current : view.pages) {
          for (const dash::PanelSpec& panel : current.panels) {
            if (panel.kind == dash::PanelKind::Alert && panel.source_id == rule.source_id)
              renderer.invalidatePanel(panel.id);
          }
        }
      }
      status_dirty = true;
      dirty = true;
    }
  }

  void resetDerivedRules(const std::set<std::string>& changed_sources,
                         std::set<std::string>& invalidated_links) {
    for (const dash::AlertRule& rule : config.alert_rules) {
      if (changed_sources.count(rule.source_id) == 0) continue;
      rule_samples.erase(rule.id);
      const auto state = rule_states.find("rule_" + rule.id);
      if (state != rule_states.end())
        dashboardResetDerivedRule(state->second, event_tracker, rule.source_id);
      if (!rule.style.linked_panel_id.empty()) invalidated_links.insert(rule.style.linked_panel_id);
      for (const dash::Page& current : view.pages) {
        for (const dash::PanelSpec& panel : current.panels) {
          if (panel.kind == dash::PanelKind::Alert && panel.source_id == rule.source_id)
            invalidated_links.insert(panel.id);
        }
      }
    }
  }

  bool setVariable(const std::string& name, const std::string& value) {
    dash::VariableValues next = overrides;
    bool found = false;
    for (auto& item : next) {
      if (item.first == name) {
        item.second = value;
        found = true;
      }
    }
    if (!found) next.emplace_back(name, value);
    dash::VariableValues resolved;
    std::string error;
    std::map<std::string, std::shared_ptr<const dp::SourceResult>> previous_results;
    for (const dash::Source& source : config.sources) {
      previous_results[source.id] = currentResult(source.id);
    }
    std::set<std::string> changed_sources;
    if (!dash::defaultVariableValues(config.variables, next, resolved, error) ||
        !pipeline->setVariables(next, error, &changed_sources)) {
      showMessage("Invalid variable");
      return false;
    }
    std::set<std::string> invalidated_links;
    auto invalidate_links = [&](const std::shared_ptr<const dp::SourceResult>& result) {
      if (result == nullptr) return;
      for (const dash::Event& event : result->events) {
        const std::string linked = linkedForEvent(event);
        if (!linked.empty()) invalidated_links.insert(linked);
      }
    };
    for (const std::string& source_id : changed_sources) {
      const auto previous = previous_results[source_id];
      clearInjectedSource(source_id);
      invalidateImageSource(source_id, previous, nullptr);
      invalidate_links(previous);
    }
    overrides = std::move(next);
    variables = std::move(resolved);
    for (const std::string& source_id : changed_sources) {
      const std::shared_ptr<const dp::SourceResult> current = currentResult(source_id);
      invalidateImageSource(source_id, previous_results[source_id], current);
      invalidate_links(current);
    }
    resetDerivedRules(changed_sources, invalidated_links);
    for (const std::string& linked : invalidated_links) renderer.invalidatePanel(linked);
    published_panels.clear();
    rebuildData();
    force_tick = true;
    dirty = true;
    status_dirty = true;
    showMessage(name + " updated");
    return true;
  }

  std::vector<std::string> variableOptions(const dash::Variable& variable) const {
    std::vector<std::string> options;
    auto add = [&](const std::string& value) {
      if (options.size() >= 64 ||
          std::find(options.begin(), options.end(), value) != options.end()) {
        return;
      }
      options.push_back(value);
    };
    if (variable.include_all) add("__all__");
    for (const dash::VariableOption& option : variable.options) add(option.value);
    for (const std::string& value : dashboardQueryValues(variable, raw_samples)) add(value);
    return options;
  }

  void cycleVariable(const std::string& name, int delta) {
    const dash::Variable* variable = nullptr;
    for (const dash::Variable& candidate : config.variables) {
      if (candidate.name == name) variable = &candidate;
    }
    if (variable == nullptr) return;
    const std::vector<std::string> options = variableOptions(*variable);
    if (options.empty()) return;
    const std::string* current = dash::findVariableValue(variables, name);
    size_t index = 0;
    if (current != nullptr) {
      for (size_t i = 0; i < options.size(); ++i) {
        if (options[i] == *current) index = i;
      }
    }
    index = (index + options.size() + static_cast<size_t>(delta)) % options.size();
    setVariable(name, options[index]);
  }

  std::vector<const dash::PanelSpec*> chartCandidates(const dash::PanelSpec& panel) const {
    std::vector<const dash::PanelSpec*> out{&panel};
    const dash::Page* owner = page(config, page_id);
    if (owner == nullptr) return out;
    for (const std::string& id : panel.linked_panel_ids) {
      for (const dash::PanelSpec& candidate : owner->panels) {
        if (candidate.visible && candidate.id == id && candidate.kind == dash::PanelKind::Chart)
          out.push_back(&candidate);
      }
    }
    return out;
  }

  void cycleChart(int delta) {
    const dash::PanelSpec* selected = focusedPanel();
    if (selected == nullptr || selected->kind != dash::PanelKind::Chart) return;
    const std::vector<const dash::PanelSpec*> candidates = chartCandidates(*selected);
    if (candidates.size() < 2) return;
    const std::string original_id = selected->id;
    std::string current = chart_selection.count(original_id) == 0
                              ? original_id
                              : chart_selection[original_id];
    size_t index = 0;
    for (size_t i = 0; i < candidates.size(); ++i) {
      if (candidates[i]->id == current) index = i;
    }
    index = (index + candidates.size() + static_cast<size_t>(delta)) % candidates.size();
    std::set<std::string> invalidated{original_id};
    for (const auto& selection : chart_selection) {
      if (dashboardResolveCarousel(chart_selection, selection.first) == original_id)
        invalidated.insert(selection.first);
    }
    chart_selection[original_id] = candidates[index]->id;
    for (const std::string& id : invalidated) {
      published_panels.erase(id);
      renderer.invalidatePanel(id);
    }
    pipeline->schedulePanel(candidates[index]->id);
    rebuildData();
    rebuildLayout();
    last_carousel_ms = monotonicMs();
    dirty = true;
    status_dirty = true;
  }

  void adjustFocused(int delta) {
    const dash::PanelSpec* selected = focusedPanel();
    if (selected == nullptr) {
      movePage(delta);
      return;
    }

    if (selected->kind == dash::PanelKind::Button && selected->actions.size() > 1) {
      action_focus = (action_focus + selected->actions.size() + static_cast<size_t>(delta)) %
                     selected->actions.size();
      invalidateFocus();
      return;
    }
    if (selected->kind == dash::PanelKind::Chart && chartCandidates(*selected).size() > 1) {
      cycleChart(delta);
      return;
    }
    for (const dash::Action& action : selected->actions) {
      if (action.kind == dash::ActionKind::SetVariable) {
        cycleVariable(action.target, delta);
        return;
      }
    }
    cycleInterval(delta);
  }

  bool refresh(const std::string& target) {
    if (!mains && battery_cycle_cancelled) {
      showMessage("Poll window ended");
      return false;
    }
    bool ok = false;
    if (target == "all") {
      for (const dash::Source& source : config.sources) ok = pipeline->refreshSource(source.id) || ok;
    } else if (target == "current") {
      if (const dash::Page* current = currentPage()) {
        std::set<std::string> sources;
        for (const dash::PanelSpec& panel : current->panels) {
          if (!panel.source_id.empty()) sources.insert(panel.source_id);
        }
        for (const std::string& id : sources) ok = pipeline->refreshSource(id) || ok;
      }
    } else {
      ok = pipeline->refreshSource(target);
    }
    if (ok) {
      force_tick = true;
      if (!mains) tick();
      showMessage("Refresh scheduled");
    } else {
      showMessage("No enabled source to refresh");
    }
    return ok;
  }

  const dash::Event* ackTarget(const std::string& target) const {
    if (target == "focused") {
      const dash::Event* event = findEvent(focused_event_id);
      return event != nullptr && event->state == dash::EventState::Active ? event : nullptr;
    }
    const dash::Event* focused_event = findEvent(focused_event_id);
    if (focused_event != nullptr && focused_event->state == dash::EventState::Active)
      return focused_event;
    const dash::Event* latest = nullptr;
    for (const ui::PanelLayout& panel : layout.panels) {
      if (panel.panel == nullptr || panel.panel->kind != dash::PanelKind::Alert) continue;
      for (const dash::Event* event : eventsForPanel(*panel.panel)) {
        if (event->state == dash::EventState::Active &&
            (latest == nullptr || event->timestamp_ms > latest->timestamp_ms)) {
          latest = event;
        }
      }
    }
    if (latest == nullptr) {
      if (const dash::PanelSpec* selected = focusedPanel()) {
        for (const dash::Event& event : view.events) {
          if (event.state == dash::EventState::Active && linkedForEvent(event) == selected->id &&
              (latest == nullptr || event.timestamp_ms > latest->timestamp_ms)) {
            latest = &event;
          }
        }
      }
    }
    return latest;
  }

  bool acknowledge(const std::string& target) {
    const dash::Event* event = ackTarget(target);
    if (event == nullptr) {
      showMessage("No active alert to acknowledge");
      return false;
    }
    return acknowledgeEvent(*event);
  }

  bool acknowledgeEvent(const dash::Event& target_event) {
    const dash::Event* event = &target_event;
    const std::string identity = dashboardEventIdentity(*event);
    const std::string core_id = dashboardCoreRecordId(*event);
    if (!core_id.empty() && deps.alerts != nullptr && deps.net != nullptr) {
      const auto snapshot = deps.alerts->acquire();
      if (snapshot != nullptr) {
        for (const alert_core::AlertRecord& record : snapshot->records) {
          if (record.id != core_id) continue;
          if (record.state != alert_core::AlertState::Active) {
            showMessage("Alert is not active");
            return false;
          }
          const auto result = deps.net->alertAction(
              {eego::net::AlertActionType::Ack, core_id, "dashboard"});
          if (result.code != eego::net::AlertActionResult::Code::Ok) {
            showMessage("Alert acknowledge failed");
            return false;
          }
          break;
        }
      }
    }
    event_tracker.acknowledge(*event, event_tracker.generation(*event));
    rebuildData();
    if (const dash::Event* changed = findEvent(identity)) {
      for (const dash::Page& current : view.pages) {
        for (const dash::PanelSpec& panel : current.panels) {
          if (panel.kind == dash::PanelKind::Alert && eventMatches(panel, *changed))
            renderer.invalidatePanel(panel.id);
        }
      }
      const std::string linked = linkedForEvent(*changed);
      if (!linked.empty()) renderer.invalidatePanel(linked);
    }
    dirty = true;
    status_dirty = true;
    showMessage("Alert acknowledged");
    return true;
  }

  bool executeAction(const std::string& panel_id, const dash::Action& action) {
    switch (action.kind) {
      case dash::ActionKind::GotoPage:
        return gotoPage(action.target);
      case dash::ActionKind::GotoPanel:
        return focusPanel(action.target);
      case dash::ActionKind::SetVariable:
        return setVariable(action.target, action.value);
      case dash::ActionKind::Refresh:
        return refresh(action.target);
      case dash::ActionKind::Back:
        if (back()) return true;
        exit_requested = true;
        return true;
      case dash::ActionKind::AckAlert:
        return acknowledge(action.target);
    }
    (void)panel_id;
    return false;
  }

  uint64_t snapshotEpoch() const {
    if (deps.alerts != nullptr) {
      const auto snapshot = deps.alerts->acquire();
      if (snapshot != nullptr) return snapshot->seq;
    }
    return alert_sequence;
  }

  void dispatchAction(const dash::PanelSpec& panel, const dash::Action& input,
                      bool confirmed = false, const DashboardConfirmRequest* confirmed_request = nullptr) {
    dash::Action action = input;
    std::string error;
    if (!dash::interpolateVariables(input.target, variables, action.target, error) ||
        !dash::interpolateVariables(input.value, variables, action.value, error)) {
      showMessage("Invalid action variable");
      return;
    }
    const bool needs_confirm = action.confirm || action.kind == dash::ActionKind::AckAlert;
    if (needs_confirm && !confirmed) {
      DashboardConfirmRequest request;
      request.panel_id = panel.id;
      request.action_id = action.id;
      request.resolved_target = action.target;
      request.resolved_value = action.value;
      request.page_epoch = page_epoch;
      request.snapshot_epoch = snapshotEpoch();
      if (action.kind == dash::ActionKind::AckAlert) {
        const dash::Event* event = ackTarget(action.target);
        if (event == nullptr) {
          showMessage("No active alert to acknowledge");
          return;
        }
        request.target_identity = dashboardEventIdentity(*event);
        request.target_generation = event_tracker.generation(*event);
      }
      if (deps.confirm_action) {
        deps.confirm_action(request);
      } else {
        showMessage("Confirmation unavailable");
      }
      return;
    }
    if (confirmed) {
      if (confirmed_request == nullptr || action.target != confirmed_request->resolved_target ||
          action.value != confirmed_request->resolved_value) {
        showMessage("Stale action cancelled");
        return;
      }
      if (action.kind == dash::ActionKind::AckAlert) {
        const dash::Event* event = ackTarget(action.target);
        if (event == nullptr || dashboardEventIdentity(*event) != confirmed_request->target_identity ||
            event_tracker.generation(*event) != confirmed_request->target_generation) {
          showMessage("Stale alert cancelled");
          return;
        }
      }
    }
    if (!executeAction(panel.id, action)) showMessage("Action unavailable");
  }

  bool executeActionRef(const std::string& panel_id, const std::string& action_id,
                        dash::ActionKind* kind) {
    const dash::PanelSpec* panel = findPanel(panel_id);
    if (panel == nullptr || !panel->visible) return false;
    for (const auto& action : panel->actions) {
      if (action.id != action_id) continue;
      if (kind != nullptr) *kind = action.kind;
      dispatchAction(*panel, action);
      return true;
    }
    return false;
  }

  bool executeConfirmed(const DashboardConfirmRequest& request) {
    if (page_epoch != request.page_epoch || snapshotEpoch() != request.snapshot_epoch) {
      showMessage("Stale action cancelled");
      return false;
    }
    const dash::PanelSpec* panel = findPanel(request.panel_id);
    if (panel == nullptr || !panel->visible) {
      showMessage("Stale action cancelled");
      return false;
    }
    for (const auto& action : panel->actions) {
      if (action.id != request.action_id) continue;
      dispatchAction(*panel, action, true, &request);
      return true;
    }
    showMessage("Stale action cancelled");
    return false;
  }

  std::string focusedControl() const {
    const dash::PanelSpec* selected = focusedPanel();
    if (selected == nullptr || selected->actions.empty()) return {};
    const size_t index = selected->kind == dash::PanelKind::Button ? action_focus : 0;
    if (index >= selected->actions.size()) return {};
    return selected->id + "/" + selected->actions[index].id;
  }

  std::string touchControl(int16_t x, int16_t y) const {
    if (y >= kStatusLineY) return {};
    ui::HitTarget target;
    if (!ui::hitTest(layout, x, y, target) || target.panel == nullptr || target.action == nullptr) {
      return {};
    }
    return target.panel->id + "/" + target.action->id;
  }

  void requestDetail(const std::string& panel_id, const std::string& identity) {
    detail_requested = true;
    detail_panel = panel_id;
    detail_identity = identity;
  }

  // 告警面板的首条（活动优先、最新优先）事件；无事件返回 nullptr。
  const dash::Event* topEvent(const dash::PanelSpec& panel) const {
    const std::vector<const dash::Event*> events = eventsForPanel(panel);
    return events.empty() ? nullptr : events.front();
  }

  // Power 短按（选择/确认）：
  //   * 已显式选中面板（焦点框可见）→ 按面板类型执行：告警面板打开详情，
  //     按钮执行动作，多图表轮换；
  //   * 未选中面板 → 本屏有告警面板则打开其告警详情；否则显示焦点框，
  //     再按一次执行（避免误触发按钮动作）。
  void activate() {
    if (!focus_visible) {
      for (const ui::PanelLayout& item : layout.panels) {
        if (item.panel == nullptr || item.panel->kind != dash::PanelKind::Alert) continue;
        if (const dash::Event* event = topEvent(*item.panel)) {
          requestDetail(item.panel->id, dashboardEventIdentity(*event));
          return;
        }
      }
      if (layout.panels.empty()) return;
      for (size_t i = 0; i < layout.panels.size(); ++i) {
        const dash::PanelSpec* candidate = layout.panels[i].panel;
        if (candidate != nullptr && !candidate->actions.empty()) {
          focus = i;
          break;
        }
      }
      focus_visible = true;
      invalidateFocus();
      return;
    }
    const dash::PanelSpec* selected = focusedPanel();
    if (selected == nullptr) return;
    if (selected->kind == dash::PanelKind::Button && !selected->actions.empty()) {
      if (action_focus >= selected->actions.size()) action_focus = 0;
      dispatchAction(*selected, selected->actions[action_focus]);
      return;
    }
    if (selected->kind == dash::PanelKind::Chart && chartCandidates(*selected).size() > 1) {
      cycleChart(1);
      return;
    }
    if (selected->kind == dash::PanelKind::Alert) {
      const dash::Event* event = findEvent(focused_event_id);
      if (event == nullptr) event = topEvent(*selected);
      if (event != nullptr) {
        requestDetail(selected->id, dashboardEventIdentity(*event));
        return;
      }
      showMessage("No alerts");
      return;
    }
    if (!selected->actions.empty()) {
      dispatchAction(*selected, selected->actions.front());
      return;
    }
    showMessage("No panel action");
  }

  void touch(int16_t x, int16_t y) {
    if (!ready) return;
    if (y >= kStatusLineY) {
      moveScreen(x < 276 ? -1 : 1);
      return;
    }
    ui::HitTarget target;
    if (ui::hitTest(layout, x, y, target) && target.panel != nullptr && target.action != nullptr) {
      const std::string old = focusedPanel() == nullptr ? "" : focusedPanel()->id;
      for (size_t i = 0; i < layout.panels.size(); ++i) {
        if (layout.panels[i].panel == target.panel) focus = i;
      }
      action_focus = target.action_index;
      focus_visible = true;
      invalidateFocus(old);
      dispatchAction(*target.panel, *target.action);
      return;
    }
    for (size_t index = layout.panels.size(); index-- > 0;) {
      const ui::PanelLayout& item = layout.panels[index];
      if (item.panel == nullptr || !item.bounds.contains(x, y)) continue;
      const std::string old = focusedPanel() == nullptr ? "" : focusedPanel()->id;
      const bool already_focused = focus == index;
      focus = index;
      action_focus = 0;

      if (item.panel->kind == dash::PanelKind::Alert) {
        // Same row geometry the renderer used to draw this panel.
        if (const dash::Event* hit = ui::alertEventAt(view, item, x, y)) {
          requestDetail(item.panel->id, dashboardEventIdentity(*hit));
          return;
        }
        const std::vector<const dash::Event*> events = ui::alertPanelEvents(view, *item.panel);
        // 标题/空白处：打开该面板的首条告警。
        if (!events.empty()) {
          requestDetail(item.panel->id, dashboardEventIdentity(*events.front()));
          return;
        }
      } else if (already_focused && item.panel->kind == dash::PanelKind::Chart) {
        cycleChart(1);
        return;
      } else {
        focused_event_id.clear();
      }
      focus_visible = true;
      invalidateFocus(old);
      return;
    }

  }

  void tick() {
    if (!ready || pipeline == nullptr) return;
    std::map<std::string, dp::SourceStatus> before;
    for (const dash::Source& source : config.sources) {
      dp::SourceStatus status;
      if (pipeline->sourceStatus(source.id, status)) before[source.id] = std::move(status);
    }

    last_tick = pipeline->tick(pipelineMs());
    last_tick_ms = monotonicMs();
    std::set<std::string> injection_links;
    const bool injections_reconciled = reconcileInjections(injection_links);
    const std::vector<dp::PanelUpdate> updates = pipeline->updates();
    const std::vector<std::string> dirty_panels = pipeline->takeDirtyPanels();
    pipeline->clearUpdates();
    for (const dp::PanelUpdate& update : updates) publishPanelUpdate(update);

    struct SourceChange {
      std::string id;
      std::shared_ptr<const dp::SourceResult> previous;
      std::shared_ptr<const dp::SourceResult> current;
      bool content_changed = false;
      bool status_changed = false;
      bool events_changed = false;
    };
    std::vector<SourceChange> changes;
    for (const dash::Source& source : config.sources) {
      const auto known = source_pointers.find(source.id);
      const auto previous_result = known == source_pointers.end()
                                       ? std::shared_ptr<const dp::SourceResult>()
                                       : known->second;
      const std::shared_ptr<const dp::SourceResult> current = pipeline->sourceResult(source.id);
      dp::SourceStatus current_status;
      if (!pipeline->sourceStatus(source.id, current_status)) continue;
      const auto published_status = source_statuses.find(source.id);
      const dp::SourceStatus& previous_status = published_status == source_statuses.end()
                                                    ? before[source.id]
                                                    : published_status->second;
      const DashboardSourceChange signal = dashboardSourceChangeOf(
          previous_result.get(), current.get(), previous_status, current_status);
      source_statuses[source.id] = current_status;
      if (!signal.content_changed && !signal.status_changed && !signal.events_changed) continue;
      changes.push_back({source.id, previous_result, current, signal.content_changed,
                         signal.status_changed, signal.events_changed});
    }

    if (injections_reconciled || !dirty_panels.empty() || !changes.empty()) {
      bool rebuild_model = injections_reconciled || !dirty_panels.empty();
      bool status_changed = false;
      for (const SourceChange& change : changes) {
        rebuild_model = rebuild_model || change.content_changed || change.events_changed;
        status_changed = status_changed || change.status_changed;
        if (change.content_changed) invalidateImageSource(change.id, change.previous, change.current);
        if (!change.events_changed) continue;
        auto invalidate_links = [&](const std::shared_ptr<const dp::SourceResult>& result) {
          if (result == nullptr) return;
          for (const dash::Event& event : result->events) {
            const std::string linked = linkedForEvent(event);
            if (!linked.empty()) renderer.invalidatePanel(linked);
          }
        };
        invalidate_links(change.previous);
        invalidate_links(change.current);
      }
      for (const std::string& linked : injection_links) renderer.invalidatePanel(linked);
      if (rebuild_model) rebuildData();
      dirty = true;
      if (status_changed) status_dirty = true;
    }
    force_tick = false;
    const int64_t wake = std::min<int64_t>(pipeline->nextWakeMs(pipelineMs()), 1000);
    next_tick_ms = last_tick_ms + std::max<int64_t>(wake, 50);
  }

  // 已提交配置变化检测：DashboardStore 由 admin 在 app_task 中 commit/erase，
  // 本函数同在 app_task，每 kStoreCheckMs 取一次快照比较修订号（无文件 IO）。
  void checkStore(int64_t now) {
    if (now - last_store_check_ms < kStoreCheckMs) return;
    last_store_check_ms = now;
    const auto document = eego::admin::DashboardStore::instance().snapshot();
    if (document == nullptr) return;
    if (!store_seen) {
      store_seen = true;
      store_revision = document->revision;
      store_configured = document->configured;
      return;
    }
    if (document->revision == store_revision && document->configured == store_configured) return;
    config_changed = true;
    store_revision = document->revision;
    store_configured = document->configured;
    if (!document->configured) {
      if (ready) unload("dashboard deleted");
      return;
    }
    if (reload()) {
      ESP_LOGI(kTag, "dashboard reloaded after commit (store revision %llu)",
               static_cast<unsigned long long>(store_revision));
    } else {
      ESP_LOGW(kTag, "dashboard reload after commit failed: %s", config_error.c_str());
    }
  }

  void unload(const char* reason) {
    if (pipeline != nullptr) pipeline->cancelAsync();
    ready = false;
    config_error = reason;
    message.clear();
    navigation.clear();
    focused_event_id.clear();
    ++page_epoch;
    renderer.reset();
    layout = ui::PageLayout{};
    ESP_LOGI(kTag, "dashboard unloaded: %s", reason);
  }

  void service(bool mains_mode) {
    mains = mains_mode;
    const int64_t now = monotonicMs();
    checkStore(now);
    if (!ready) return;
    if (!message.empty() && now > message_deadline_ms) {
      message.clear();
      status_dirty = true;
      dirty = true;
    }

    if (deps.alerts != nullptr) {
      const auto snapshot = deps.alerts->acquire();
      if (snapshot != nullptr && snapshot->seq != alert_sequence) {
        alert_sequence = snapshot->seq;
        rebuildData();
        for (const dash::Page& current : view.pages) {
          for (const dash::PanelSpec& panel : current.panels) {
            if (panel.kind == dash::PanelKind::Alert) renderer.invalidatePanel(panel.id);
          }
        }
        std::set<std::string> current_links;
        for (const alert_core::AlertRecord& record : snapshot->records) {
          if (!dashboardAlertRecordVisible(record)) continue;
          const std::string linked = linkedForRecord(record);
          if (!linked.empty()) current_links.insert(linked);
        }
        for (const std::string& linked : dashboardAlertLinkUnion(alert_core_links, current_links))
          renderer.invalidatePanel(linked);
        alert_core_links = std::move(current_links);
        dirty = true;
        status_dirty = true;
      }
    }

    if (!mains && battery_cycle_active && !battery_cycle_cancelled &&
        now >= battery_cycle_deadline_ms) {
      pipeline->cancelAsync();
      battery_cycle_cancelled = true;
    }
    const bool pump = mains ? (force_tick || now >= next_tick_ms) : !pipeline->asyncIdle();
    if (pump) tick();
    if (now - last_rule_eval_ms >= 1000) {
      last_rule_eval_ms = now;
      evaluateRules();
    }

    if (const dash::PanelSpec* selected = focusedPanel();
        selected != nullptr && selected->kind == dash::PanelKind::Chart &&
        chartCandidates(*selected).size() > 1 && now - last_carousel_ms >= kCarouselPeriodMs) {
      cycleChart(1);
    }
    if (now - last_status_ms >= kStatusCheckMs) {
      last_status_ms = now;
      if (now - last_clock_query_ms >= kStatusPeriodMs) {
        last_clock_query_ms = now;
        refreshClockLabel();
      }
      std::string ignored;
      if (composeStatus(ignored) != drawn_status_text) {
        status_dirty = true;
        dirty = true;
      }
    }
  }

  void pollBattery(int64_t budget_ms) {
    if (!dashboardPipelineReady(ready, pipeline.get())) return;
    battery_cycle_active = true;
    battery_cycle_cancelled = false;
    battery_cycle_deadline_ms = monotonicMs() + budget_ms;
    pipeline->resumeAsync();
    tick();
  }

  void cancelBatteryCycle() {
    if (!dashboardPipelineReady(ready, pipeline.get())) return;
    pipeline->cancelAsync();  // 取消在途请求并挂起：直到 resumeAsync 不再发起新请求
    battery_cycle_active = false;
    battery_cycle_cancelled = true;
  }

  void setBatteryStatus(const std::string& text) {
    if (text == battery_status) return;
    battery_status = text;
    status_dirty = true;
    dirty = true;
  }

  bool networkIdle() const {
    return !ready || pipeline == nullptr || pipeline->asyncIdle();
  }

  void setMains(bool value) {
    mains = value;
    battery_cycle_active = false;
    battery_cycle_cancelled = false;
    if (mains && dashboardPipelineReady(ready, pipeline.get())) {
      pipeline->resumeAsync();
      force_tick = true;
    }
    status_dirty = true;
    dirty = true;
  }

  const dash::AlertRule* ruleForEvent(const dash::Event& event) const {
    for (const dash::AlertRule& rule : config.alert_rules) {
      if (rule.id == event.rule_id) return &rule;
    }
    return nullptr;
  }

  const dash::Event* bannerEvent() const {
    return dashboardBannerEvent(view.events, config.alert_rules, event_tracker);
  }

  void updateBannerDirty() {
    const std::string signature = dashboardBannerSignature(view.events, config.alert_rules, event_tracker);
    if (signature == banner_signature) return;
    banner_signature = signature;
    status_dirty = true;
    dirty = true;
  }

  void drawAlertMarkers() {
    for (const ui::PanelLayout& item : layout.panels) {
      if (item.panel == nullptr || item.panel->kind == dash::PanelKind::Alert ||
          item.panel->kind == dash::PanelKind::Button) {
        continue;
      }
      const dash::Event* newest = nullptr;
      for (const dash::Event& event : view.events) {
        if (event.state != dash::EventState::Active || event_tracker.suppressed(event, event_tracker.generation(event)) ||
            linkedForEvent(event) != item.panel->id) {
          continue;
        }
        const dash::AlertRule* rule = ruleForEvent(event);
        uint8_t surfaces = dash::kAlertSurfaceCard;
        if (rule != nullptr) {
          surfaces = rule->style.surfaces;
        } else if (item.panel->kind == dash::PanelKind::Table) {
          surfaces = dash::kAlertSurfaceTableRow;
        } else if (item.panel->kind == dash::PanelKind::Chart) {
          surfaces = dash::kAlertSurfaceSparkline;
        }
        const bool visible =
            (item.panel->kind == dash::PanelKind::Card &&
             (surfaces & dash::kAlertSurfaceCard) != 0) ||
            (item.panel->kind == dash::PanelKind::Table &&
             (surfaces & dash::kAlertSurfaceTableRow) != 0) ||
            (item.panel->kind == dash::PanelKind::Chart &&
             (surfaces & dash::kAlertSurfaceSparkline) != 0);
        if (visible && (newest == nullptr || event.timestamp_ms > newest->timestamp_ms))
          newest = &event;
      }
      if (newest == nullptr) continue;
      const uint8_t gray = newest->severity == dash::Severity::Critical
                               ? 0
                               : newest->severity == dash::Severity::Warning ? 5 : 10;
      deps.canvas->fillRoundRectGray(item.bounds.right() - 12, item.bounds.y + 3, 8, 14, 2, gray);
    }
  }

  void drawFocus() {
    if (!focus_visible) return;
    const ui::PanelLayout* selected = focusedLayout();
    if (selected == nullptr || selected->panel == nullptr) return;
    const ui::Rect& bounds = selected->bounds;
    if (bounds.w > 4 && bounds.h > 4)
      deps.canvas->drawRect(bounds.x + 1, bounds.y + 1, bounds.w - 2, bounds.h - 2, 0);
    if (selected->panel->kind == dash::PanelKind::Button && selected->panel->actions.size() > 1) {
      const ui::Rect action = ui::actionBounds(*selected, selected->panel->actions.size(), action_focus);
      if (action.w > 4 && action.h > 4)
        deps.canvas->drawRect(action.x + 1, action.y + 1, action.w - 2, action.h - 2, 0);
    }
  }

  std::string banner_event_id;
  std::string banner_signature;

  // 底部状态行：位于普通 UI 安全区（28..740）内的最后 kStatusLineH 像素。
  // 布局区（rebuildLayout 的 LayoutConfig.content）与触控阈值须同步让出这段高度。
  static constexpr int16_t kStatusLineH = 22;
  static constexpr int16_t kStatusLineY = 28 + 712 - kStatusLineH;  // 718

  // 时钟来源标签（SNTP/RTC/NOCLK）：queryStatus 会经 net_task 命令队列，
  // 仅在 service() 中按 kStatusPeriodMs 刷新缓存，绘制时不再阻塞查询。
  void refreshClockLabel() {
    if (!TimeSource::valid()) {
      clock_label = "NOCLK";
      return;
    }
    eego::net::NetStatusSnapshot net_status;
    if (deps.net != nullptr && deps.net->queryStatus(net_status, 50)) {
      clock_label = net_status.time.sntp_synced ? "SNTP" : "RTC";
    } else if (std::string_view(clock_label) == "NOCLK") {
      clock_label = "RTC";
    }
  }

  std::string composeStatus(std::string& banner_id) {
    banner_id.clear();
    if (!message.empty()) return message;
    if (const dash::Event* event = bannerEvent()) {
      banner_id = dashboardEventIdentity(*event);
      return std::string(dash::severityName(event->severity)) + " " + event->title + "  >";
    }
    if (!battery_status.empty()) return battery_status;
    const std::vector<const dash::Page*> pages = visiblePages();
    size_t number = 0;
    for (size_t i = 0; i < pages.size(); ++i) {
      if (pages[i]->id == page_id) number = i + 1;
    }
    size_t cached = 0;
    size_t offline = 0;
    if (pipeline != nullptr) {
      for (const dash::Source& source : config.sources) {
        dp::SourceStatus status;
        if (!pipeline->sourceStatus(source.id, status)) continue;
        if (pipeline->sourceResult(source.id) != nullptr) ++cached;
        if (status.offline || status.stale) ++offline;
      }
    }
    const char* clock = TimeSource::valid() ? clock_label : "NOCLK";
    if (TimeSource::valid() && std::string_view(clock) == "NOCLK") clock = "RTC";
    char status[160];
    char screen[24];
    if (layout.page_count > 1) {
      std::snprintf(screen, sizeof(screen), "%u/%u.%u", static_cast<unsigned>(number),
                    static_cast<unsigned>(pages.size()), static_cast<unsigned>(page_index + 1));
    } else {
      std::snprintf(screen, sizeof(screen), "%u/%u", static_cast<unsigned>(number),
                    static_cast<unsigned>(pages.size()));
    }
    std::snprintf(status, sizeof(status), "%s  %s  DATA %u/%u  OFF %u", screen, clock,
                  static_cast<unsigned>(cached - std::min(cached, offline)),
                  static_cast<unsigned>(config.sources.size()), static_cast<unsigned>(offline));
    return status;
  }

  void drawStatus() {
    deps.canvas->fillRect(28, kStatusLineY, 496, kStatusLineH, 1);
    deps.canvas->drawLine(28, kStatusLineY, 28 + 496 - 1, kStatusLineY, 0);
    const std::string text = composeStatus(banner_event_id);
    drawn_status_text = text;
    const int16_t text_h = eego::FontManager::instance().uiFontHeight();
    const int16_t text_y = static_cast<int16_t>(
        kStatusLineY + 1 + std::max(0, (kStatusLineH - 1 - text_h) / 2));
    ui::drawTextFit(*deps.canvas, eego::FontManager::instance(), 28 + 12, text_y, 496 - 24, text,
                    1, false, 0);
  }

  bool renderPreview(eego::PortraitCanvas& canvas, const dash::Dashboard& candidate,
                     const std::string& wanted_page, size_t screen,
                     DashboardRuntime::PreviewInfo& info) const {
    dash::Dashboard preview = candidate;
    const ui::PreviewData data =
        ui::fillPreviewData(preview, ready ? &view : nullptr, pipelineMs());
    info.data = ui::previewDataName(data);
    ui::PreviewRenderOptions options;
    options.page_id = wanted_page;
    options.screen = screen;
    options.layout_config = layoutConfig();
    // No network image provider: a preview never fetches remote assets (SD-local
    // images still load through the renderer's built-in SD provider).
    const ui::PreviewRenderResult result = ui::renderPreviewPage(canvas, preview, options);
    info.bad_request = result.bad_request;
    info.error = result.error;
    info.page_id = result.page_id;
    info.screen = result.screen;
    info.screen_count = result.screen_count;
    if (!result.ok) return false;
    // Status line like the live dashboard (divider + page/screen), marked as preview.
    canvas.drawLine(28, kStatusLineY, 28 + 496 - 1, kStatusLineY, 0);
    std::string title = info.page_id;
    for (const dash::Page& item : preview.pages) {
      if (item.id == info.page_id && !item.title.empty()) title = item.title;
    }
    const std::string text = "Preview  " + title + "  " + std::to_string(info.screen + 1) + "/" +
                             std::to_string(info.screen_count);
    const int16_t text_h = eego::FontManager::instance().uiFontHeight();
    const int16_t text_y = static_cast<int16_t>(
        kStatusLineY + 1 + std::max(0, (kStatusLineH - 1 - text_h) / 2));
    ui::drawTextFit(canvas, eego::FontManager::instance(), 28 + 12, text_y, 496 - 24, text, 1,
                    false, 0);
    return true;
  }

  bool needsRender() const {
    return ready && dirty && monotonicMs() - last_render_ms >= kRenderThrottleMs;
  }

  // push=false：只画进 framebuffer（电池模式先算帧哈希再决定是否/如何刷屏）。
  void render(bool push = true) {
    if (!ready || deps.canvas == nullptr) return;
    rebuildLayout();
    ui::RenderOptions options;
    options.page_id = page_id;
    options.page_index = page_index;
    options.variables = variables;
    options.force_full = force_full;
    options.layout_config = layoutConfig();
    last_render = renderer.render(*deps.canvas, view, options);
    drawAlertMarkers();
    drawFocus();
    drawStatus();
    deps.canvas->resetClip();
    if (!push) {
      ++skipped_render_count;
    } else if (dashboardShouldDisplay(last_render.refresh_needed, status_dirty)) {
      const eego::RefreshMode mode = force_full ? eego::RefreshMode::Full : last_render.refresh_mode;
      deps.canvas->display(mode);
      deps.canvas->waitRefreshComplete();
      ++render_count;
    } else {
      ++skipped_render_count;
    }
    force_full = false;
    dirty = false;
    status_dirty = false;
    last_render_ms = monotonicMs();
  }

  void invalidateAll() {
    dirty = true;
    force_full = true;
    status_dirty = true;
  }

  bool setInterval(const std::string& id, uint32_t interval) {
    if (findPanel(id) == nullptr || !validInterval(interval)) return false;
    std::string error;
    if (!pipeline->setPanelRefreshOverride(id, interval, error)) return false;
    if (interval == 0) {
      refresh_overrides.erase(id);
    } else {
      refresh_overrides[id] = interval;
    }
    force_tick = true;
    status_dirty = true;
    dirty = true;
    return savePrefs();
  }

  bool injectData(const std::string& source_id, const std::string& body, size_t& metrics,
                  size_t& events, std::string& error) {
    const dash::Source* source = nullptr;
    for (const dash::Source& candidate : config.sources) {
      if (candidate.id == source_id) source = &candidate;
    }
    if (source == nullptr) {
      error = "unknown source";
      return false;
    }
    dp::SourcePlan plan;
    bool found = false;
    for (const dp::SourcePlan& candidate : plans) {
      if (candidate.source.id == source_id) {
        plan = candidate;
        found = true;
      }
    }
    if (!found) plan.source = *source;
    dp::HttpResponse response;
    response.status = 200;
    response.body = body;
    dp::SourceResult result;
    if (!dp::parseSourceResponse(plan, response, variables, pipelineMs(), result, error)) return false;
    const std::shared_ptr<const dp::SourceResult> previous = currentResult(source_id);
    const auto replacement = std::make_shared<const dp::SourceResult>(std::move(result));
    invalidateImageSource(source_id, previous, replacement);
    std::set<std::string> invalidated_links;
    auto invalidate_links = [&](const std::shared_ptr<const dp::SourceResult>& effective) {
      if (effective == nullptr) return;
      for (const dash::Event& event : effective->events) {
        const std::string linked = linkedForEvent(event);
        if (!linked.empty()) invalidated_links.insert(linked);
      }
    };
    invalidate_links(previous);
    invalidate_links(replacement);
    injected[source_id] = replacement;
    source_pointers[source_id] = pipeline->sourceResult(source_id);
    for (const dash::Page& current : view.pages) {
      for (const dash::PanelSpec& panel : current.panels) {
        if (panel.source_id != source_id) continue;
        published_panels.erase(panel.id);
        renderer.invalidatePanel(panel.id);
      }
    }
    for (const std::string& linked : invalidated_links) renderer.invalidatePanel(linked);
    rebuildData();
    metrics = replacement->metrics.size();
    events = replacement->events.size();
    dirty = true;
    status_dirty = true;
    return true;
  }

  static bool sensitiveName(std::string name) {
    return dashboardSensitiveVariableName(std::move(name));
  }

  void diagStatus() {
    if (!ready) {
      printf("dashboard: loaded=0 (onboarding screen) reason=%s\n", config_error.c_str());
      return;
    }
    printf("dashboard: loaded=1 id=%s revision=%u page=%s subpage=%u focus=%s\n",
           config.id.c_str(), static_cast<unsigned>(revision), page_id.c_str(),
           static_cast<unsigned>(page_index + 1),
           focusedPanel() == nullptr ? "-" : focusedPanel()->id.c_str());
    printf("dashboard: pages=%u panels=%u sources=%u samples=%u events=%u cache=%d power=%s\n",
           static_cast<unsigned>(config.pages.size()),
           static_cast<unsigned>(layout.panels.size()),
           static_cast<unsigned>(config.sources.size()),
           static_cast<unsigned>(view.samples.size()), static_cast<unsigned>(view.events.size()),
           cache != nullptr ? 1 : 0, mains ? "mains" : "battery");
    printf("dashboard: history_points=%u raw_points=%u time_valid=%d clock_ms=%lld\n",
           static_cast<unsigned>(pipeline->history().pointCount()),
           static_cast<unsigned>(pipeline->history().rawPointCount()), TimeSource::valid() ? 1 : 0,
           static_cast<long long>(pipelineMs()));
    printf("dashboard: tick due=%u http=%u dedup=%u ok=%u fail=%u dirty=%u\n",
           static_cast<unsigned>(last_tick.due_sources), static_cast<unsigned>(last_tick.http_requests),
           static_cast<unsigned>(last_tick.deduplicated_requests),
           static_cast<unsigned>(last_tick.succeeded), static_cast<unsigned>(last_tick.failed),
           static_cast<unsigned>(last_tick.dirty_panels));
    printf("dashboard: renders=%u no_change=%u last_panels=%u fast_full=%d image_cache=%u\n",
           static_cast<unsigned>(render_count), static_cast<unsigned>(skipped_render_count),
           static_cast<unsigned>(last_render.drawn_panels),
           last_render.refresh_mode == eego::RefreshMode::Full ? 1 : 0,
           static_cast<unsigned>(last_render.image_cache_bytes));
    for (const dash::Source& source : config.sources) {
      dp::SourceStatus status;
      if (!pipeline->sourceStatus(source.id, status)) continue;
      printf("  source %s enabled=%d cached=%d offline=%d stale=%d failures=%u http=%d cache_error=%d\n",
             source.id.c_str(), source.enabled ? 1 : 0,
             pipeline->sourceResult(source.id) != nullptr ? 1 : 0, status.offline ? 1 : 0,
             status.stale ? 1 : 0, static_cast<unsigned>(status.consecutive_failures),
             status.last_http_status, status.cache_error.empty() ? 0 : 1);
    }
  }

  void diagVariables() {
    if (!ready) {
      printf("ERR dashboard: not loaded\n");
      return;
    }
    printf("dashboard variables: %u\n", static_cast<unsigned>(config.variables.size()));
    for (const dash::Variable& variable : config.variables) {
      const std::string* value = dash::findVariableValue(variables, variable.name);
      const std::string shown = sensitiveName(variable.name)
                                    ? "<redacted>"
                                    : ui::sanitizeText(value == nullptr ? "" : *value, false, 80);
      printf("  %s kind=%s value=%s options=%u\n", variable.name.c_str(),
             dash::variableKindName(variable.kind), shown.c_str(),
             static_cast<unsigned>(variable.options.size()));
    }
  }

  void diagPanels() {
    if (!ready) {
      printf("ERR dashboard: not loaded\n");
      return;
    }
    for (const dash::Page& current : view.pages) {
      if (!current.visible) continue;
      for (const dash::PanelSpec& panel : current.panels) {
        if (!panel.visible) continue;
        const std::string metric = published_panels.metric(panel.id, panel.metric);
        printf("  panel %s page=%s kind=%s source=%s metric=%s refresh=%ums override=%d%s\n",
               panel.id.c_str(), current.id.c_str(), dash::panelKindName(panel.kind),
               panel.source_id.empty() ? "-" : panel.source_id.c_str(),
               metric.empty() ? "-" : metric.c_str(),
               static_cast<unsigned>(effectiveInterval(panel)),
               refresh_overrides.count(panel.id) != 0 ? 1 : 0,
               focusedPanel() != nullptr && focusedPanel()->id == panel.id ? " focused" : "");
      }
    }
  }

  void diag(const char* args) {
    std::string input = args == nullptr ? "" : args;
    size_t start = input.find_first_not_of(' ');
    if (start == std::string::npos) {
      diagStatus();
      return;
    }
    input.erase(0, start);
    size_t end = input.find(' ');
    const std::string verb = input.substr(0, end);
    std::string rest = end == std::string::npos ? "" : input.substr(end + 1);
    start = rest.find_first_not_of(' ');
    if (start != std::string::npos && start > 0) rest.erase(0, start);

    if (verb == "status") {
      diagStatus();
    } else if (verb == "vars" || verb == "variables") {
      diagVariables();
    } else if (verb == "panels") {
      diagPanels();
    } else if (verb == "reload") {
      if (!reload()) printf("ERR dashboard reload: invalid or unavailable bundle\n");
    } else if (!ready) {
      printf("ERR dashboard: not loaded\n");
    } else if (verb == "refresh") {
      refresh(rest.empty() ? "current" : rest);
    } else if (verb == "page") {
      if (!gotoPage(rest)) printf("ERR dashboard page: unknown page\n");
    } else if (verb == "panel") {
      if (!focusPanel(rest)) printf("ERR dashboard panel: unknown panel\n");
    } else if (verb == "var") {
      end = rest.find(' ');
      if (end == std::string::npos) {
        printf("ERR usage: dashboard var <name> <value>\n");
      } else {
        setVariable(rest.substr(0, end), rest.substr(end + 1));
      }
    } else if (verb == "interval") {
      end = rest.find(' ');
      char* parse_end = nullptr;
      const uint32_t seconds = end == std::string::npos
                                   ? 0
                                   : static_cast<uint32_t>(std::strtoul(rest.c_str() + end + 1,
                                                                       &parse_end, 10));
      if (end == std::string::npos || parse_end == rest.c_str() + end + 1 ||
          (seconds != 0 && seconds != 60 && seconds != 300 && seconds != 900 && seconds != 1800)) {
        printf("ERR usage: dashboard interval <panel> <0|60|300|900|1800 seconds>\n");
      } else if (!setInterval(rest.substr(0, end), seconds * 1000)) {
        printf("ERR dashboard interval: unknown panel or NVS save failed\n");
      } else {
        printf("dashboard interval: saved\n");
      }
    } else if (verb == "inject" || verb == "data") {
      end = rest.find(' ');
      if (end == std::string::npos) {
        printf("ERR usage: dashboard inject <source> <standard-json>\n");
      } else {
        size_t metrics = 0;
        size_t events = 0;
        std::string error;
        if (injectData(rest.substr(0, end), rest.substr(end + 1), metrics, events, error)) {
          printf("dashboard inject: metrics=%u events=%u (RAM only)\n",
                 static_cast<unsigned>(metrics), static_cast<unsigned>(events));
        } else {
          printf("ERR dashboard inject: %s\n", ui::sanitizeText(error, false, 100).c_str());
        }
      }
    } else if (verb == "smoke") {
      smoke();
    } else {
      printf("ERR usage: dashboard status|vars|panels|page <id>|panel <id>|var <name> <value>|"
             "refresh [all|current|source]|interval <panel> <seconds>|inject <source> <json>|"
             "reload|smoke\n");
    }
  }

  bool smoke() {
    size_t passed = 0;
    size_t failed = 0;
    auto check = [&](bool ok, const char* name) {
      printf("[%s] dashboard-%s\n", ok ? "PASS" : "FAIL", name);
      ok ? ++passed : ++failed;
    };
    check(ready && pipeline != nullptr, "config");
    if (!ready) {
      printf("dashboard smoke: pass=%u fail=%u\n", static_cast<unsigned>(passed),
             static_cast<unsigned>(failed));
      return false;
    }
    check(currentPage() != nullptr && layout.valid, "layout");
    dash::VariableValues checked_variables;
    std::string variable_error;
    check(dash::defaultVariableValues(config.variables, overrides, checked_variables, variable_error),
          "variables");
    check(!eego::SdCard::instance().ready() || cache != nullptr, "cache");
    bool actions_valid = true;
    for (const dash::Page& current : config.pages) {
      for (const dash::PanelSpec& panel : current.panels) {
        for (const dash::Action& action : panel.actions) {
          if (std::string_view(dash::actionKindName(action.kind)) == "unknown") actions_valid = false;
        }
      }
    }
    check(actions_valid, "actions");
    check(deps.canvas != nullptr && deps.net != nullptr, "wiring");
    printf("dashboard smoke: pass=%u fail=%u (output carries no secrets)\n",
           static_cast<unsigned>(passed), static_cast<unsigned>(failed));
    return failed == 0;
  }

  bool exit_requested = false;
};

DashboardRuntime::DashboardRuntime() : impl_(new (std::nothrow) Impl) {}
DashboardRuntime::~DashboardRuntime() = default;

bool DashboardRuntime::begin(const Deps& deps) { return impl_ != nullptr && impl_->begin(deps); }
bool DashboardRuntime::reload() { return impl_ != nullptr && impl_->reload(); }
bool DashboardRuntime::available() const { return impl_ != nullptr && impl_->ready; }
std::string DashboardRuntime::unavailableReason() const {
  return impl_ == nullptr ? std::string("out of memory") : impl_->config_error;
}
std::string DashboardRuntime::title() const {
  return impl_ == nullptr || !impl_->ready ? std::string() : impl_->config.title;
}
void DashboardRuntime::service(bool mains) {
  if (impl_ != nullptr) impl_->service(mains);
}
void DashboardRuntime::pollBattery(int64_t budget_ms) {
  if (impl_ != nullptr) impl_->pollBattery(budget_ms);
}
void DashboardRuntime::cancelBatteryCycle() {
  if (impl_ != nullptr) impl_->cancelBatteryCycle();
}
void DashboardRuntime::setBatteryStatus(const std::string& text) {
  if (impl_ != nullptr) impl_->setBatteryStatus(text);
}
void DashboardRuntime::renderOffscreen() {
  if (impl_ != nullptr) impl_->render(false);
}
bool DashboardRuntime::networkIdle() const {
  return impl_ == nullptr || impl_->networkIdle();
}
void DashboardRuntime::setMains(bool mains) {
  if (impl_ != nullptr) impl_->setMains(mains);
}
void DashboardRuntime::invalidateAll() {
  if (impl_ != nullptr) impl_->invalidateAll();
}
bool DashboardRuntime::needsRender() const { return impl_ != nullptr && impl_->needsRender(); }
void DashboardRuntime::render() {
  if (impl_ != nullptr) impl_->render();
}
void DashboardRuntime::moveFocus(int delta, bool adjust) {
  if (impl_ == nullptr || !impl_->ready) return;
  if (adjust) {
    impl_->adjustFocused(delta);
  } else {
    impl_->moveFocus(delta);
  }
}
void DashboardRuntime::movePage(int delta) {
  if (impl_ != nullptr && impl_->ready) impl_->movePage(delta);
}
void DashboardRuntime::moveScreen(int delta) {
  if (impl_ != nullptr && impl_->ready) impl_->moveScreen(delta);
}
void DashboardRuntime::home() {
  if (impl_ != nullptr && impl_->ready) impl_->home();
}
bool DashboardRuntime::atHome() const {
  return impl_ == nullptr || !impl_->ready || impl_->atHome();
}
bool DashboardRuntime::firstScreen() const {
  if (impl_ == nullptr || !impl_->ready) return true;
  const std::vector<const eego::dashboard::Page*> pages = impl_->visiblePages();
  return impl_->page_index == 0 && (pages.empty() || pages.front()->id == impl_->page_id);
}
void DashboardRuntime::refreshCurrent() {
  if (impl_ != nullptr && impl_->ready) impl_->refresh("current");
}
void DashboardRuntime::refreshAll() {
  if (impl_ != nullptr && impl_->ready) impl_->refresh("all");
}
void DashboardRuntime::setGlobalRefresh(uint32_t refresh_ms) {
  if (impl_ != nullptr) impl_->setGlobalRefresh(refresh_ms);
}
void DashboardRuntime::activate() {
  if (impl_ != nullptr && impl_->ready) impl_->activate();
}
void DashboardRuntime::touch(int16_t x, int16_t y) {
  if (impl_ == nullptr || !impl_->ready) return;
  if (y >= Impl::kStatusLineY && !impl_->banner_event_id.empty()) {
    if (impl_->findEvent(impl_->banner_event_id) != nullptr) {
      impl_->requestDetail({}, impl_->banner_event_id);
      return;
    }
  }
  impl_->touch(x, y);
}
bool DashboardRuntime::back() { return impl_ != nullptr && impl_->back(); }
bool DashboardRuntime::consumeExitRequest() {
  if (impl_ == nullptr || !impl_->exit_requested) return false;
  impl_->exit_requested = false;
  return true;
}
bool DashboardRuntime::gotoPage(const std::string& page_id) {
  return impl_ != nullptr && impl_->gotoPage(page_id);
}
bool DashboardRuntime::gotoPanel(const std::string& panel_id) {
  return impl_ != nullptr && impl_->focusPanel(panel_id);
}
bool DashboardRuntime::focusAction(const std::string& panel_id, const std::string& action_id,
                                   const std::string& event_id) {
  if (impl_ == nullptr || !impl_->ready || !impl_->focusPanel(panel_id)) return false;
  const dash::PanelSpec* selected = impl_->focusedPanel();
  if (selected == nullptr) return false;
  for (size_t i = 0; i < selected->actions.size(); ++i) {
    if (selected->actions[i].id != action_id) continue;
    impl_->action_focus = i;
    impl_->focused_event_id = event_id;
    impl_->invalidateFocus();
    return true;
  }
  return false;
}
bool DashboardRuntime::executeActionRef(const std::string& panel_id,
                                        const std::string& action_id,
                                        eego::dashboard::ActionKind* kind) {
  return impl_ != nullptr && impl_->ready &&
         impl_->executeActionRef(panel_id, action_id, kind);
}
bool DashboardRuntime::executeConfirmed(const DashboardConfirmRequest& request) {
  return impl_ != nullptr && impl_->ready && impl_->executeConfirmed(request);
}
uint64_t DashboardRuntime::pageEpoch() const {
  return impl_ == nullptr ? 0 : impl_->page_epoch;
}
uint64_t DashboardRuntime::snapshotEpoch() const {
  return impl_ == nullptr ? 0 : impl_->snapshotEpoch();
}
std::string DashboardRuntime::pageContext() const {
  return impl_ == nullptr || !impl_->ready ? "dashboard" : "dashboard:" + impl_->page_id;
}
std::string DashboardRuntime::focusedControl() const {
  return impl_ == nullptr || !impl_->ready ? std::string() : impl_->focusedControl();
}
bool DashboardRuntime::renderPreview(eego::PortraitCanvas& canvas,
                                     const eego::dashboard::Dashboard& candidate,
                                     const std::string& page_id, size_t screen,
                                     PreviewInfo& info) const {
  if (impl_ == nullptr) {
    info.error = "dashboard runtime unavailable";
    return false;
  }
  return impl_->renderPreview(canvas, candidate, page_id, screen, info);
}

std::string DashboardRuntime::touchControl(int16_t x, int16_t y) const {
  return impl_ == nullptr || !impl_->ready ? std::string() : impl_->touchControl(x, y);
}
void DashboardRuntime::showStatusMessage(const std::string& text, uint32_t duration_ms) {
  if (impl_ != nullptr && impl_->ready) impl_->showMessage(text, duration_ms);
}
std::string DashboardRuntime::linkedPanelForAlert(const alert_core::AlertRecord& record) const {
  return impl_ == nullptr || !impl_->ready ? std::string() : impl_->linkedForRecord(record);
}
bool DashboardRuntime::consumeDetailRequest(std::string& panel_id, std::string& identity) {
  if (impl_ == nullptr || !impl_->detail_requested) return false;
  impl_->detail_requested = false;
  panel_id = std::move(impl_->detail_panel);
  identity = std::move(impl_->detail_identity);
  impl_->detail_panel.clear();
  impl_->detail_identity.clear();
  return impl_->ready;
}

std::vector<DashboardAlertItem> DashboardRuntime::alertItems(const std::string& panel_id) const {
  std::vector<DashboardAlertItem> out;
  if (impl_ == nullptr || !impl_->ready) return out;
  std::vector<const dash::Event*> events;
  if (!panel_id.empty()) {
    const dash::PanelSpec* panel = impl_->findPanel(panel_id);
    if (panel == nullptr) return out;
    events = impl_->eventsForPanel(*panel);
  } else {
    events.reserve(impl_->view.events.size());
    for (const dash::Event& event : impl_->view.events) events.push_back(&event);
    ui::orderAlertEvents(events);  // same order + cap as the panel lists
  }
  out.reserve(events.size());
  for (const dash::Event* event : events) {
    DashboardAlertItem item;
    item.identity = dashboardEventIdentity(*event);
    item.core_id = dashboardCoreRecordId(*event);
    item.title = event->title;
    item.message = event->message;
    item.source_id = event->source_id;
    item.rule_id = event->rule_id;
    item.linked_panel_id = impl_->linkedForEvent(*event);
    item.severity = static_cast<uint8_t>(event->severity);
    item.acknowledged = impl_->acknowledged_events.count(item.identity) != 0;
    item.active = event->state == dash::EventState::Active || item.acknowledged;
    item.timestamp_ms = event->timestamp_ms;
    out.push_back(std::move(item));
  }
  return out;
}

std::vector<std::string> DashboardRuntime::activeCriticalIdentities() const {
  std::vector<std::string> out;
  if (impl_ == nullptr || !impl_->ready) return out;
  for (const dash::Event& event : impl_->view.events) {
    if (event.severity != dash::Severity::Critical || event.state != dash::EventState::Active) {
      continue;
    }
    std::string identity = dashboardEventIdentity(event);
    if (impl_->acknowledged_events.count(identity) != 0) continue;
    out.push_back(std::move(identity));
  }
  return out;
}

bool DashboardRuntime::consumeConfigChanged() {
  if (impl_ == nullptr || !impl_->config_changed) return false;
  impl_->config_changed = false;
  return true;
}

bool DashboardRuntime::acknowledgeEvent(const std::string& identity) {
  if (impl_ == nullptr || !impl_->ready) return false;
  const dash::Event* event = impl_->findEvent(identity);
  if (event == nullptr || event->state != dash::EventState::Active) return false;
  const dash::Event copy = *event;
  return impl_->acknowledgeEvent(copy);
}

std::vector<DashboardSourceInfo> DashboardRuntime::sources() const {
  std::vector<DashboardSourceInfo> out;
  if (impl_ == nullptr || !impl_->ready || impl_->pipeline == nullptr) return out;
  for (const dash::Source& source : impl_->config.sources) {
    DashboardSourceInfo info;
    info.id = source.id;
    info.enabled = source.enabled;
    info.has_data = impl_->currentResult(source.id) != nullptr;
    dp::SourceStatus status;
    if (impl_->pipeline->sourceStatus(source.id, status)) {
      info.offline = status.offline;
      info.stale = status.stale;
      info.consecutive_failures = status.consecutive_failures;
      info.last_http_status = status.last_http_status;
      info.last_success_ms = status.last_success_ms;
      info.last_attempt_ms = status.last_attempt_ms;
      info.error = ui::sanitizeText(status.error.empty() ? status.cache_error : status.error, false, 80);
    }
    out.push_back(std::move(info));
  }
  return out;
}

void DashboardRuntime::diag(const char* args) {
  if (impl_ != nullptr) impl_->diag(args);
}
bool DashboardRuntime::smoke() { return impl_ != nullptr && impl_->smoke(); }

}  // namespace panel
