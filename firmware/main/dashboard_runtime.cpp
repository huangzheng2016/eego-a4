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

#include "alert_core/snapshot.h"
#include "alert_core/types.h"
#include "dashboard_core/config.h"
#include "dashboard_core/expression.h"
#include "dashboard_core/json.h"
#include "dashboard_ui/layout.h"
#include "dashboard_ui/renderer.h"
#include "dashboard_ui/text.h"
#include "data_pipeline/data_pipeline.h"
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
constexpr int64_t kStatusPeriodMs = 30000;
constexpr int64_t kCarouselPeriodMs = 15000;
constexpr int64_t kConfirmMs = 5000;

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
    const dash::ParseResult parsed = dash::parseDashboard(text);
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
  return event.id + "\n" + std::to_string(event.timestamp_ms) + "\n" +
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

  struct PendingAction {
    dash::Action action;
    std::string panel_id;
    int64_t deadline_ms = 0;
    bool armed = false;
  };

  struct RuleState {
    bool active = false;
    int64_t true_since_ms = -1;
    std::string event_id;
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

  dash::VariableValues overrides;
  dash::VariableValues variables;
  std::map<std::string, uint32_t> refresh_overrides;
  std::map<std::string, std::string> chart_selection;
  std::map<std::string, RuleState> rule_states;
  std::set<std::string> acked_events;
  std::map<std::string, std::shared_ptr<const dp::SourceResult>> injected;
  std::map<std::string, std::shared_ptr<const dp::SourceResult>> source_pointers;

  std::string page_id;
  size_t page_index = 0;
  size_t focus = 0;
  size_t action_focus = 0;
  std::string focused_event_id;
  ui::PageLayout layout;
  std::vector<Location> navigation;
  PendingAction pending;

  bool dirty = true;
  bool force_full = true;
  bool status_dirty = true;
  bool force_tick = true;
  bool mains = true;
  int64_t last_tick_ms = 0;
  int64_t next_tick_ms = 0;
  int64_t last_render_ms = -10000;
  int64_t last_status_ms = -100000;
  int64_t last_rule_eval_ms = -100000;
  int64_t last_carousel_ms = 0;
  uint64_t alert_sequence = 0;
  dp::TickResult last_tick;
  ui::RenderResult last_render;
  std::string message;
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
        if (!panel.visible || panel.id != id) continue;
        if (owner != nullptr) *owner = &candidate;
        if (subpage != nullptr) *subpage = panel.grid.y / ui::kRowsPerPage;
        return &panel;
      }
    }
    return nullptr;
  }

  void rebuildLayout() {
    const dash::Page* current = currentPage();
    if (current == nullptr) {
      layout = ui::PageLayout{};
      focus = 0;
      return;
    }
    layout = ui::buildPageLayout(*current, page_index);
    if (layout.page_count == 0) page_index = 0;
    if (page_index >= layout.page_count) {
      page_index = layout.page_count - 1;
      layout = ui::buildPageLayout(*current, page_index);
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
    for (const auto& item : refresh_overrides) {
      std::string ignored;
      candidate->setPanelRefreshOverride(item.first, item.second, ignored);
    }

    pipeline = std::move(candidate);
    config = std::move(next_config);
    plans = std::move(next_plans);
    overrides = std::move(next_overrides);
    variables = std::move(next_variables);
    revision = next_revision;
    ready = true;
    config_error.clear();
    source_pointers.clear();
    injected.clear();
    rule_states.clear();
    acked_events.clear();
    navigation.clear();
    focused_event_id.clear();
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
    pending.armed = false;
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
    if (findPanel(id, &owner, &subpage) == nullptr || owner == nullptr) return false;
    setPage(owner->id, subpage, remember);
    for (size_t i = 0; i < layout.panels.size(); ++i) {
      if (layout.panels[i].panel != nullptr && layout.panels[i].panel->id == id) {
        focus = i;
        break;
      }
    }
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
    index = (index + pages.size() + static_cast<size_t>(delta)) % pages.size();
    setPage(pages[index]->id, 0, false);
  }

  void moveFocus(int delta) {
    if (layout.panels.empty()) {
      movePage(delta);
      return;
    }
    const std::string old = focusedPanel() == nullptr ? "" : focusedPanel()->id;
    const int64_t next = static_cast<int64_t>(focus) + delta;
    if (next < 0) {
      if (page_index > 0) {
        --page_index;
        rebuildLayout();
        focus = layout.panels.empty() ? 0 : layout.panels.size() - 1;
        force_full = true;
      } else {
        movePage(-1);
        return;
      }
    } else if (next >= static_cast<int64_t>(layout.panels.size())) {
      if (page_index + 1 < layout.page_count) {
        ++page_index;
        rebuildLayout();
        focus = 0;
        force_full = true;
      } else {
        movePage(1);
        return;
      }
    } else {
      focus = static_cast<size_t>(next);
    }
    pending.armed = false;
    focused_event_id.clear();
    action_focus = 0;
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
    view.events.push_back(event);
    if (acked_events.count(event.id) != 0) view.events.back().state = dash::EventState::Cleared;
  }

  void rebuildData() {
    if (!ready || pipeline == nullptr) return;
    view = config;

    for (dash::Page& current : view.pages) {
      for (dash::PanelSpec& panel : current.panels) {
        if (panel.kind != dash::PanelKind::Chart) continue;
        const auto selected = chart_selection.find(panel.id);
        if (selected == chart_selection.end() || selected->second == panel.id) continue;
        for (const dash::PanelSpec& candidate : current.panels) {
          if (candidate.id != selected->second || candidate.kind != dash::PanelKind::Chart) continue;
          panel.source_id = candidate.source_id;
          panel.metric = candidate.metric;
          panel.value_path = candidate.value_path;
          panel.value_expression = candidate.value_expression;
          panel.time_window = candidate.time_window;
          panel.thresholds = candidate.thresholds;
          panel.style = candidate.style;
          panel.chart = candidate.chart;
          break;
        }
      }
    }

    view.samples.clear();
    view.events.clear();
    std::set<std::string> seen_samples;
    std::set<std::string> seen_events;
    for (const dash::MetricSample& sample : config.samples) appendSample(sample, seen_samples);
    for (const dash::Event& event : config.events) appendEvent(event, seen_events);

    std::map<std::pair<std::string, std::string>, const dash::PanelSpec*> series;
    for (const dash::Page& current : view.pages) {
      for (const dash::PanelSpec& panel : current.panels) {
        if (!panel.visible || panel.source_id.empty() || panel.metric.empty()) continue;
        const auto key = std::make_pair(panel.source_id, panel.metric);
        if (series.count(key) == 0) series[key] = &panel;
      }
    }
    const int64_t now = pipelineMs();
    for (const auto& item : series) {
      const dash::PanelSpec& panel = *item.second;
      int64_t from = 0;
      int64_t to = now;
      if (!dash::parseTimeExpression(panel.time_window.from, now, from) ||
          !dash::parseTimeExpression(panel.time_window.to, now, to)) {
        from = std::max<int64_t>(0, now - 3600000);
        to = now;
      }
      const std::vector<dash::MetricSample> samples = pipeline->history().query(
          panel.source_id, panel.metric, {}, {std::min(from, to), std::max(from, to)});
      for (const dash::MetricSample& sample : samples) appendSample(sample, seen_samples);
    }

    std::vector<std::shared_ptr<const dp::SourceResult>> results;
    for (const dash::Source& source : config.sources) {
      std::shared_ptr<const dp::SourceResult> current = pipeline->sourceResult(source.id);
      const auto known = source_pointers.find(source.id);
      if (known == source_pointers.end() || known->second != current) {
        injected.erase(source.id);
        source_pointers[source.id] = current;
      }
      const auto local = injected.find(source.id);
      if (local != injected.end()) current = local->second;
      if (current == nullptr) continue;
      results.push_back(current);
      for (const dash::MetricSample& sample : current->metrics) appendSample(sample, seen_samples);
      for (const dash::Event& event : current->events) appendEvent(event, seen_events);
    }

    if (deps.alerts != nullptr) {
      const auto snapshot = deps.alerts->acquire();
      if (snapshot != nullptr) {
        for (const alert_core::AlertRecord& record : snapshot->records) {
          dash::Event event;
          event.id = record.id;
          event.rule_id = record.type;
          event.source_id = record.source.id;
          event.timestamp_ms = static_cast<int64_t>(record.last_seen) * 1000;
          event.title = record.title.empty() ? record.type : record.title;
          event.message = record.message;
          event.linked_panel_id = linkedForRecord(record);
          switch (record.severity) {
            case alert_core::Severity::Critical: event.severity = dash::Severity::Critical; break;
            case alert_core::Severity::Warning: event.severity = dash::Severity::Warning; break;
            default: event.severity = dash::Severity::Info; break;
          }
          event.state = record.state == alert_core::AlertState::Active
                            ? dash::EventState::Active
                            : dash::EventState::Cleared;
          appendEvent(event, seen_events);
        }
      }
    }

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
      for (auto sample = view.samples.rbegin(); sample != view.samples.rend(); ++sample) {
        if (sample->source_id == rule->source_id && sample->metric == rule->metric) {
          event.timestamp_ms = sample->timestamp_ms;
          break;
        }
      }
      appendEvent(event, seen_events);
    }
    rebuildLayout();
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

  std::vector<const dash::Event*> eventsForPanel(const dash::PanelSpec& panel) const {
    std::vector<const dash::Event*> out;
    for (const dash::Event& event : view.events) {
      if (out.size() >= 256) break;
      if (eventMatches(panel, event)) out.push_back(&event);
    }
    std::stable_sort(out.begin(), out.end(), [](const dash::Event* left, const dash::Event* right) {
      if (left->state != right->state) return left->state == dash::EventState::Active;
      return left->timestamp_ms > right->timestamp_ms;
    });
    return out;
  }

  const dash::Event* findEvent(std::string_view id) const {
    if (id.empty()) return nullptr;
    const dash::Event* latest = nullptr;
    for (const dash::Event& event : view.events) {
      if (event.id == id &&
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
      const dash::MetricSample* latest = nullptr;
      for (const dash::MetricSample& sample : view.samples) {
        if (sample.source_id != rule.source_id || sample.metric != rule.metric) continue;
        if (latest == nullptr || sample.timestamp_ms >= latest->timestamp_ms) latest = &sample;
      }
      if (latest == nullptr) continue;
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
          state.event_id = "rule_" + rule.id;
          acked_events.erase(state.event_id);
          changed = true;
        }
      } else {
        state.true_since_ms = -1;
        if (state.active) {
          state.active = false;
          acked_events.erase(state.event_id);
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
    if (!dash::defaultVariableValues(config.variables, next, resolved, error) ||
        !pipeline->setVariables(next, error)) {
      showMessage("Invalid variable");
      return false;
    }
    overrides = std::move(next);
    variables = std::move(resolved);
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
    if (variable.kind == dash::VariableKind::Query) {
      std::string_view path(variable.value_path);
      if (path.substr(0, 2) == "$.") path.remove_prefix(2);
      if (path.substr(0, 7) == "labels.") path.remove_prefix(7);
      const size_t separator = path.find_last_of("./");
      if (separator != std::string_view::npos) path.remove_prefix(separator + 1);
      for (const dash::MetricSample& sample : view.samples) {
        if (sample.source_id != variable.source_id) continue;
        if (path == "metric") {
          add(sample.metric);
        } else if (path == "source" || path == "source_id") {
          add(sample.source_id);
        } else {
          for (const dash::KeyValue& label : sample.labels) {
            if (label.key == path) add(label.value);
          }
        }
      }
    }
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
    const dash::Page* owner = currentPage();
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
    chart_selection[original_id] = candidates[index]->id;
    rebuildData();
    rebuildLayout();
    renderer.invalidatePanel(original_id);
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
    pending.armed = false;
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
    const std::string id = event->id;
    if (deps.alerts != nullptr && deps.net != nullptr) {
      const auto snapshot = deps.alerts->acquire();
      if (snapshot != nullptr) {
        for (const alert_core::AlertRecord& record : snapshot->records) {
          if (record.id != id) continue;
          if (record.state != alert_core::AlertState::Active) {
            showMessage("Alert is not active");
            return false;
          }
          const auto result = deps.net->alertAction(
              {eego::net::AlertActionType::Ack, id, "dashboard"});
          if (result.code != eego::net::AlertActionResult::Code::Ok) {
            showMessage("Alert acknowledge failed");
            return false;
          }
          break;
        }
      }
    }
    acked_events.insert(id);
    rebuildData();
    if (const dash::Event* changed = findEvent(id)) {
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

  void dispatchAction(const dash::PanelSpec& panel, const dash::Action& input) {
    dash::Action action = input;
    std::string error;
    if (!dash::interpolateVariables(input.target, variables, action.target, error) ||
        !dash::interpolateVariables(input.value, variables, action.value, error)) {
      showMessage("Invalid action variable");
      return;
    }
    const int64_t now = monotonicMs();
    const bool same = pending.armed && pending.panel_id == panel.id &&
                      pending.action.id == action.id && now <= pending.deadline_ms;
    if (action.confirm || action.kind == dash::ActionKind::AckAlert) {
      if (!same) {
        pending = {action, panel.id, now + kConfirmMs, true};
        showMessage("Press again to confirm", kConfirmMs);
        return;
      }
    }
    pending.armed = false;
    if (!executeAction(panel.id, action)) showMessage("Action unavailable");
  }

  void activate() {
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
      if (event == nullptr) {
        const std::vector<const dash::Event*> events = eventsForPanel(*selected);
        if (!events.empty()) event = events.front();
      }
      if (event != nullptr) {
        const std::string linked = linkedForEvent(*event);
        if (!linked.empty()) {
          const std::string event_id = event->id;
          if (focusPanel(linked)) focused_event_id = event_id;
          return;
        }
      }
    }
    if (!selected->actions.empty()) {
      dispatchAction(*selected, selected->actions.front());
      return;
    }
    showMessage("No panel action");
  }

  void touch(int16_t x, int16_t y) {
    if (!ready) return;
    if (y >= 740) {
      movePage(x < 276 ? -1 : 1);
      return;
    }
    ui::HitTarget target;
    if (ui::hitTest(layout, x, y, target) && target.panel != nullptr && target.action != nullptr) {
      for (size_t i = 0; i < layout.panels.size(); ++i) {
        if (layout.panels[i].panel == target.panel) focus = i;
      }
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
      pending.armed = false;
      if (item.panel->kind == dash::PanelKind::Alert) {
        ui::Rect body = item.content;
        if (!item.panel->title.empty()) {
          const int16_t title_height = eego::FontManager::instance().uiFontHeight() + 5;
          body.y += title_height;
          body.h -= title_height;
        }
        const int16_t row_height = std::max<int16_t>(
            eego::FontManager::instance().uiFontHeight() + 7, 14);
        const std::vector<const dash::Event*> events = eventsForPanel(*item.panel);
        if (body.h > 0 && y >= body.y) {
          const size_t row = static_cast<size_t>((y - body.y) / row_height);
          if (row < events.size()) {
            const std::string event_id = events[row]->id;
            focused_event_id = event_id;
            const std::string linked = linkedForEvent(*events[row]);
            if (!linked.empty()) {
              if (focusPanel(linked)) focused_event_id = event_id;
              return;
            }
          }
        }
      } else if (already_focused && item.panel->kind == dash::PanelKind::Chart) {
        cycleChart(1);
        return;
      } else {
        focused_event_id.clear();
      }
      invalidateFocus(old);
      return;
    }
    pending.armed = false;
  }

  void tick() {
    if (!ready || pipeline == nullptr) return;
    last_tick = pipeline->tick(pipelineMs());
    last_tick_ms = monotonicMs();
    const std::vector<std::string> dirty_panels = pipeline->takeDirtyPanels();
    pipeline->clearUpdates();
    if (!dirty_panels.empty()) {
      rebuildData();
      for (const std::string& id : dirty_panels) renderer.invalidatePanel(id);
      dirty = true;
      status_dirty = true;
    }
    force_tick = false;
    const int64_t wake = std::min<int64_t>(pipeline->nextWakeMs(pipelineMs()), 1000);
    next_tick_ms = last_tick_ms + std::max<int64_t>(wake, 50);
  }

  void service(bool mains_mode) {
    if (!ready) return;
    mains = mains_mode;
    const int64_t now = monotonicMs();
    if (pending.armed && now > pending.deadline_ms) {
      pending.armed = false;
      message.clear();
      status_dirty = true;
      dirty = true;
    }
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
        for (const alert_core::AlertRecord& record : snapshot->records) {
          const std::string linked = linkedForRecord(record);
          if (!linked.empty()) renderer.invalidatePanel(linked);
        }
        dirty = true;
        status_dirty = true;
      }
    }

    if (mains && (force_tick || now >= next_tick_ms)) tick();
    if (now - last_rule_eval_ms >= 1000) {
      last_rule_eval_ms = now;
      evaluateRules();
    }

    if (const dash::PanelSpec* selected = focusedPanel();
        selected != nullptr && selected->kind == dash::PanelKind::Chart &&
        chartCandidates(*selected).size() > 1 && now - last_carousel_ms >= kCarouselPeriodMs) {
      cycleChart(1);
    }
    if (now - last_status_ms >= kStatusPeriodMs) {
      last_status_ms = now;
      status_dirty = true;
      dirty = true;
    }
  }

  void pollBattery() {
    if (ready) tick();
  }

  void setMains(bool value) {
    mains = value;
    if (mains) force_tick = true;
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
    const dash::Event* selected = nullptr;
    for (const dash::Event& event : view.events) {
      if (event.state != dash::EventState::Active || acked_events.count(event.id) != 0) continue;
      const dash::AlertRule* rule = ruleForEvent(event);
      if (rule == nullptr || (rule->style.surfaces & dash::kAlertSurfaceBanner) == 0) continue;
      if (selected == nullptr || event.timestamp_ms > selected->timestamp_ms ||
          (event.timestamp_ms == selected->timestamp_ms &&
           static_cast<uint8_t>(event.severity) > static_cast<uint8_t>(selected->severity))) {
        selected = &event;
      }
    }
    return selected;
  }

  void drawAlertMarkers() {
    for (const ui::PanelLayout& item : layout.panels) {
      if (item.panel == nullptr || item.panel->kind == dash::PanelKind::Alert ||
          item.panel->kind == dash::PanelKind::Button) {
        continue;
      }
      const dash::Event* newest = nullptr;
      for (const dash::Event& event : view.events) {
        if (event.state != dash::EventState::Active || acked_events.count(event.id) != 0 ||
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

  void drawStatus() {
    banner_event_id.clear();
    deps.canvas->fillRect(28, 742, 496, 22, 1);
    std::string text;
    if (!message.empty()) {
      text = message;
    } else if (const dash::Event* event = bannerEvent()) {
      banner_event_id = event->id;
      text = std::string(dash::severityName(event->severity)) + " " + event->title;
      if (!linkedForEvent(*event).empty()) text += "  [TREND]";
    } else {
      const std::vector<const dash::Page*> pages = visiblePages();
      size_t number = 0;
      for (size_t i = 0; i < pages.size(); ++i) {
        if (pages[i]->id == page_id) number = i + 1;
      }
      size_t cached = 0;
      size_t offline = 0;
      for (const dash::Source& source : config.sources) {
        dp::SourceStatus status;
        if (!pipeline->sourceStatus(source.id, status)) continue;
        if (pipeline->sourceResult(source.id) != nullptr) ++cached;
        if (status.offline || status.stale) ++offline;
      }
      const char* clock = TimeSource::valid() ? "CLOCK" : "NOCLK";
      if (TimeSource::valid()) {
        eego::net::NetStatusSnapshot net_status;
        if (deps.net != nullptr && deps.net->queryStatus(net_status, 50) &&
            net_status.time.sntp_synced) {
          clock = "SNTP";
        } else {
          clock = "RTC";
        }
      }
      char status[160];
      std::snprintf(status, sizeof(status), "P%u/%u.%u %s DATA %u/%u OFF %u  U/D NAV PWR OPEN",
                    static_cast<unsigned>(number), static_cast<unsigned>(pages.size()),
                    static_cast<unsigned>(page_index + 1), clock,
                    static_cast<unsigned>(cached - std::min(cached, offline)),
                    static_cast<unsigned>(config.sources.size()), static_cast<unsigned>(offline));
      text = status;
    }
    ui::drawTextFit(*deps.canvas, eego::FontManager::instance(), 30, 746, 492, text, 1, false, 0);
  }

  bool needsRender() const {
    return ready && dirty && monotonicMs() - last_render_ms >= kRenderThrottleMs;
  }

  void render() {
    if (!ready || deps.canvas == nullptr) return;
    rebuildLayout();
    ui::RenderOptions options;
    options.page_id = page_id;
    options.page_index = page_index;
    options.variables = variables;
    options.force_full = force_full;
    last_render = renderer.render(*deps.canvas, view, options);
    drawAlertMarkers();
    drawFocus();
    drawStatus();
    deps.canvas->resetClip();
    if (last_render.refresh_needed || status_dirty) {
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
    injected[source_id] = std::make_shared<const dp::SourceResult>(std::move(result));
    rebuildData();
    for (const dash::Page& current : view.pages) {
      for (const dash::PanelSpec& panel : current.panels) {
        if (panel.source_id == source_id) renderer.invalidatePanel(panel.id);
      }
    }
    metrics = injected[source_id]->metrics.size();
    events = injected[source_id]->events.size();
    dirty = true;
    status_dirty = true;
    return true;
  }

  static bool sensitiveName(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    for (const char* marker : {"token", "password", "passwd", "secret", "api_key", "apikey",
                               "authorization", "credential"}) {
      if (name.find(marker) != std::string::npos) return true;
    }
    return false;
  }

  void diagStatus() {
    if (!ready) {
      printf("dashboard: loaded=0 (legacy alert UI active)\n");
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
        printf("  panel %s page=%s kind=%s source=%s metric=%s refresh=%ums override=%d%s\n",
               panel.id.c_str(), current.id.c_str(), dash::panelKindName(panel.kind),
               panel.source_id.empty() ? "-" : panel.source_id.c_str(),
               panel.metric.empty() ? "-" : panel.metric.c_str(),
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
void DashboardRuntime::service(bool mains) {
  if (impl_ != nullptr) impl_->service(mains);
}
void DashboardRuntime::pollBattery() {
  if (impl_ != nullptr) impl_->pollBattery();
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
void DashboardRuntime::activate() {
  if (impl_ != nullptr && impl_->ready) impl_->activate();
}
void DashboardRuntime::touch(int16_t x, int16_t y) {
  if (impl_ == nullptr || !impl_->ready) return;
  if (y >= 740 && !impl_->banner_event_id.empty()) {
    if (const dash::Event* event = impl_->findEvent(impl_->banner_event_id)) {
      const std::string linked = impl_->linkedForEvent(*event);
      if (!linked.empty()) {
        const std::string event_id = event->id;
        if (impl_->focusPanel(linked)) impl_->focused_event_id = event_id;
        return;
      }
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
std::string DashboardRuntime::linkedPanelForAlert(const alert_core::AlertRecord& record) const {
  return impl_ == nullptr || !impl_->ready ? std::string() : impl_->linkedForRecord(record);
}
void DashboardRuntime::diag(const char* args) {
  if (impl_ != nullptr) impl_->diag(args);
}
bool DashboardRuntime::smoke() { return impl_ != nullptr && impl_->smoke(); }

}  // namespace panel
