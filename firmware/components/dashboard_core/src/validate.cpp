// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cctype>
#include <cmath>
#include <string_view>

#include "dashboard_core/config.h"
#include "dashboard_core/expression.h"

#include "config_internal.h"

namespace eego::dashboard {
namespace {

std::string child(const std::string& parent, const std::string& name) {
  return parent.empty() ? name : parent + "." + name;
}

bool validId(std::string_view id) {
  if (id.empty() || id.size() > kMaxIdBytes) return false;
  for (const char c : id) {
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-'))
      return false;
  }
  return true;
}

bool validVariableName(std::string_view name) {
  if (name.empty() || name.size() > kMaxIdBytes ||
      !((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z') ||
        name[0] == '_'))
    return false;
  for (const char c : name) {
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '_'))
      return false;
  }
  return true;
}

bool validHttpUrl(std::string_view url) {
  std::string_view rest;
  if (url.substr(0, 7) == "http://") rest = url.substr(7);
  else if (url.substr(0, 8) == "https://") rest = url.substr(8);
  else return false;
  if (rest.empty()) return false;
  const size_t authority_end = rest.find_first_of("/?#");
  const std::string_view authority = rest.substr(0, authority_end);
  if (authority.empty() || authority.find('@') != std::string_view::npos) return false;
  for (const unsigned char c : url) {
    if (c <= 0x20 || c == 0x7f) return false;
  }
  return true;
}

bool hasParentPathSegment(std::string_view path) {
  size_t start = 0;
  while (start <= path.size()) {
    const size_t end = path.find_first_of("/\\", start);
    const std::string_view segment = path.substr(start, end - start);
    if (segment == "..") return true;
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return false;
}

bool validAssetUri(std::string_view uri) {
  if (uri.empty()) return false;
  if (validHttpUrl(uri)) return true;
  const bool sd_uri = uri.substr(0, 3) == "sd:";
  if (!sd_uri && uri.front() != '/') return false;
  if ((sd_uri && uri.size() == 3) || uri.size() < 2) return false;
  for (const unsigned char c : uri) {
    if (c <= 0x20 || c == 0x7f) return false;
  }
  return !hasParentPathSegment(sd_uri ? uri.substr(3) : uri);
}

bool asciiEqual(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) return false;
  for (size_t i = 0; i < left.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(left[i])) !=
        std::tolower(static_cast<unsigned char>(right[i])))
      return false;
  }
  return true;
}

bool validHeaderName(std::string_view name) {
  if (name.empty()) return false;
  for (const unsigned char c : name) {
    const bool ascii_alnum = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                             (c >= 'a' && c <= 'z');
    if (!(ascii_alnum || c == '!' || c == '#' || c == '$' || c == '%' || c == '&' ||
          c == '\'' || c == '*' || c == '+' || c == '-' || c == '.' || c == '^' || c == '_' ||
          c == '`' || c == '|' || c == '~'))
      return false;
  }
  return true;
}

bool validHeaderValue(std::string_view value) {
  for (const unsigned char c : value) {
    if ((c <= 0x1f && c != '\t') || c == 0x7f) return false;
  }
  return true;
}

bool validLocale(std::string_view locale) {
  if (locale.size() < 2 || locale.size() > 32) return false;
  for (const char c : locale) {
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_'))
      return false;
  }
  return true;
}

bool validSha256(std::string_view digest) {
  if (digest.empty()) return true;
  if (digest.size() != 64) return false;
  for (const char c : digest) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
      return false;
  }
  return true;
}

struct PanelLocation {
  const PanelSpec* panel;
  const Page* page;
  std::string path;
};

class Validator {
 public:
  Validator(const Dashboard& dashboard, const Limits& limits, std::vector<ValidationError>& errors)
      : dashboard_(dashboard), limits_(limits), errors_(errors) {}

  void run() {
    check(validId(dashboard_.id), "id", "must match [A-Za-z0-9_-]{1,64}");
    check(dashboard_.title.size() <= kMaxTitleBytes, "title", "exceeds size limit");
    check(dashboard_.refresh_ms >= 1000 && dashboard_.refresh_ms <= 86400000, "refresh_ms",
          "must be in 1000..86400000");
    count(dashboard_.sources.size(), limits_.max_sources, "sources");
    count(dashboard_.variables.size(), limits_.max_variables, "variables");
    count(dashboard_.pages.size(), limits_.max_pages, "pages");
    count(dashboard_.alert_rules.size(), limits_.max_alert_rules, "alert_rules");
    count(dashboard_.assets.size(), limits_.max_assets, "assets");
    count(dashboard_.samples.size(), limits_.max_samples, "samples");
    count(dashboard_.events.size(), limits_.max_events, "events");
    check(!dashboard_.pages.empty(), "pages", "at least one page is required");

    collectSources();
    collectVariables();
    collectPages();
    collectAssets();
    collectAlertRules();
    collectSamples();
    collectEvents();

    check(containsPage(dashboard_.default_page_id), "default_page_id", "unknown page ID");
    resolveTemplates();
  }

 private:
  void check(bool condition, const std::string& field, const std::string& message) {
    if (!condition) errors_.push_back({field, message});
  }

  void count(size_t actual, size_t maximum, const std::string& field) {
    check(actual <= maximum, field, "exceeds maximum item count " + std::to_string(maximum));
  }

  template <typename T>
  static std::vector<std::string> ids(const std::vector<T>& values) {
    std::vector<std::string> result;
    for (const auto& value : values) result.push_back(value.id);
    return result;
  }

  template <typename T>
  bool uniqueIds(const std::vector<T>& values, const std::string& collection) {
    bool valid = true;
    for (size_t i = 0; i < values.size(); ++i) {
      const std::string path = collection + "[" + std::to_string(i) + "].id";
      check(validId(values[i].id), path, "must match [A-Za-z0-9_-]{1,64}");
      for (size_t j = 0; j < i; ++j) {
        if (values[i].id == values[j].id) {
          check(false, path, "duplicate ID: " + values[i].id);
          valid = false;
        }
      }
    }
    return valid;
  }

  static bool contains(const std::vector<std::string>& values, const std::string& id) {
    return std::find(values.begin(), values.end(), id) != values.end();
  }

  bool containsSource(const std::string& id) const { return contains(ids(sources_), id); }
  bool containsPage(const std::string& id) const { return contains(ids(dashboard_.pages), id); }
  bool containsAlert(const std::string& id) const { return contains(ids(dashboard_.alert_rules), id); }
  bool isImageAsset(const std::string& id) const {
    for (const auto& asset : dashboard_.assets) {
      if (asset.id == id) return asset.kind == AssetKind::Image;
    }
    return false;
  }
  bool containsPanel(const std::string& id) const {
    for (const auto& location : panels_) {
      if (location.panel->id == id) return true;
    }
    return false;
  }
  bool containsVariable(const std::string& name) const {
    for (const auto& variable : dashboard_.variables) {
      if (variable.name == name) return true;
    }
    return false;
  }

  void headers(const std::vector<KeyValue>& values, const std::string& path) {
    count(values.size(), kMaxHeaders, path);
    for (size_t i = 0; i < values.size(); ++i) {
      const std::string field = path + "." + values[i].key;
      check(validHeaderName(values[i].key), field, "invalid HTTP header name");
      check(!asciiEqual(values[i].key, "authorization") && !asciiEqual(values[i].key, "host") &&
                !asciiEqual(values[i].key, "content-length"),
            field, "header is managed by the runtime and cannot be configured");
      check(validHeaderValue(values[i].value), field,
            "HTTP header value contains a forbidden control character");
      check(values[i].value.size() <= 512, field, "exceeds 512 bytes");
      for (size_t j = 0; j < i; ++j)
        check(!asciiEqual(values[i].key, values[j].key), field, "duplicate HTTP header");
    }
  }

  void labels(const std::vector<KeyValue>& values, const std::string& path) {
    count(values.size(), kMaxLabels, path);
    for (size_t i = 0; i < values.size(); ++i) {
      check(!values[i].key.empty() && values[i].key.size() <= 64, path + "." + values[i].key,
            "invalid label key");
      check(values[i].value.size() <= 512, path + "." + values[i].key, "label value too long");
      for (size_t j = 0; j < i; ++j)
        check(values[i].key != values[j].key, path + "." + values[i].key, "duplicate label key");
    }
  }

  void collectSources() {
    uniqueIds(dashboard_.sources, "sources");
    for (size_t i = 0; i < dashboard_.sources.size(); ++i) {
      const Source& source = dashboard_.sources[i];
      const std::string path = "sources[" + std::to_string(i) + "]";
      check(source.id != "all" && source.id != "current", child(path, "id"),
            "all and current are reserved refresh targets");
      check(std::string_view(sourceKindName(source.kind)) != "unknown", child(path, "kind"),
            "invalid source kind");
      check(source.refresh_ms >= 1000 && source.refresh_ms <= 86400000, child(path, "refresh_ms"),
            "invalid refresh interval");
      check(source.timeout_ms >= 100 && source.timeout_ms <= 120000, child(path, "timeout_ms"),
            "invalid timeout");
      check(source.max_response_bytes >= 1024 && source.max_response_bytes <= 262144,
            child(path, "max_response_bytes"), "invalid response budget");
      if (source.kind == SourceKind::Http)
        check(validHttpUrl(source.url), child(path, "url"), "must be an absolute HTTP(S) URL without userinfo");
      else
        check(source.url.empty(), child(path, "url"), "static sources cannot have a URL");
      headers(source.headers, child(path, "headers"));
    }
  }

  void collectVariables() {
    for (size_t i = 0; i < dashboard_.variables.size(); ++i) {
      const Variable& variable = dashboard_.variables[i];
      const std::string path = "variables[" + std::to_string(i) + "]";
      check(validVariableName(variable.name), child(path, "name"),
            "must match [A-Za-z_][A-Za-z0-9_]{0,63}");
      for (size_t j = 0; j < i; ++j)
        check(variable.name != dashboard_.variables[j].name, child(path, "name"), "duplicate variable name");
      check(std::string_view(variableKindName(variable.kind)) != "unknown", child(path, "kind"),
            "invalid variable kind");
      count(variable.options.size(), limits_.max_variable_options, child(path, "options"));
      check(variable.default_value.size() <= kMaxVariableValueBytes, child(path, "default_value"),
            "exceeds size limit");
      for (size_t j = 0; j < variable.options.size(); ++j) {
        check(variable.options[j].value.size() <= kMaxVariableValueBytes,
              child(path, "options") + "[" + std::to_string(j) + "].value", "exceeds size limit");
        for (size_t k = 0; k < j; ++k)
          check(variable.options[j].value != variable.options[k].value,
                child(path, "options") + "[" + std::to_string(j) + "].value", "duplicate option value");
      }
      if (variable.kind == VariableKind::Query) {
        check(containsSource(variable.source_id), child(path, "source_id"), "query variable requires a known source");
        check(!variable.value_path.empty(), child(path, "value_path"), "query variable requires a value path");
      } else {
        check(variable.source_id.empty(), child(path, "source_id"), "only query variables can have a source");
      }
      if (variable.kind == VariableKind::TimeRange && !variable.default_value.empty()) {
        int64_t parsed = 0;
        check(parseTimeExpression(variable.default_value, 0, parsed), child(path, "default_value"),
              "invalid relative time range");
      }
      if (variable.kind == VariableKind::Locale)
        check(validLocale(variable.default_value), child(path, "default_value"), "invalid locale identifier");
    }
  }

  void expression(const std::string& text, const std::string& path) {
    if (text.empty()) return;
    ExpressionError error;
    if (!validateExpressionSyntax(text, error))
      check(false, path, error.message + " at expression byte " + std::to_string(error.offset));
  }

  void window(const TimeWindow& value, const std::string& path) {
    int64_t from = 0;
    int64_t to = 0;
    const bool valid_from = parseTimeExpression(value.from, 0, from);
    const bool valid_to = parseTimeExpression(value.to, 0, to);
    check(valid_from, child(path, "from"), "expected now, now-duration, or a duration");
    check(valid_to, child(path, "to"), "expected now, now-duration, or a duration");
    if (valid_from && valid_to) check(from <= to, path, "window start must not be after its end");
  }

  void action(const Action& action, const std::string& path) {
    check(validId(action.id), child(path, "id"), "invalid action ID");
    check(std::string_view(actionKindName(action.kind)) != "unknown", child(path, "kind"),
          "invalid action kind");
    const bool uses_value = action.kind == ActionKind::SetVariable;
    check(uses_value || action.value.empty(), child(path, "value"),
          "value is only valid for set_variable");
    switch (action.kind) {
      case ActionKind::GotoPage:
        check(containsPage(action.target), child(path, "target"), "unknown target page");
        break;
      case ActionKind::GotoPanel:
        check(containsPanel(action.target), child(path, "target"), "unknown target panel");
        break;
      case ActionKind::SetVariable:
        check(containsVariable(action.target), child(path, "target"), "unknown target variable");
        check(action.value.size() <= kMaxVariableValueBytes, child(path, "value"), "exceeds size limit");
        break;
      case ActionKind::Refresh:
        // Data poll only: all enabled sources, current-page sources, or one source.
        check(action.target == "all" || action.target == "current" || containsSource(action.target),
              child(path, "target"), "expected all, current, or a registered source ID");
        break;
      case ActionKind::Back:
        check(action.target.empty(), child(path, "target"), "back does not accept a target");
        break;
      case ActionKind::AckAlert:
        check(action.target == "current" || action.target == "focused", child(path, "target"),
              "ack_alert target must be current or focused");
        break;
    }
  }

  void collectPages() {
    uniqueIds(dashboard_.pages, "pages");
    size_t total_panels = 0;
    for (size_t i = 0; i < dashboard_.pages.size(); ++i) {
      const Page& page = dashboard_.pages[i];
      const std::string path = "pages[" + std::to_string(i) + "]";
      count(page.panels.size(), limits_.max_panels_per_page, child(path, "panels"));
      total_panels += page.panels.size();
      check(page.grid_columns >= 1 && page.grid_columns <= 64, child(path, "grid_columns"),
            "must be in 1..64");
      check(page.row_height >= 1 && page.row_height <= 256, child(path, "row_height"),
            "must be in 1..256");
      for (size_t j = 0; j < page.panels.size(); ++j) {
        const PanelSpec& panel = page.panels[j];
        const std::string panel_path = child(path, "panels") + "[" + std::to_string(j) + "]";
        panels_.push_back({&panel, &page, panel_path});
      }
    }
    count(total_panels, limits_.max_total_panels, "pages");

    for (size_t i = 0; i < panels_.size(); ++i) {
      const PanelSpec& panel = *panels_[i].panel;
      const Page& page = *panels_[i].page;
      const std::string& path = panels_[i].path;
      check(validId(panel.id), child(path, "id"), "invalid panel ID");
      for (size_t j = 0; j < i; ++j)
        check(panel.id != panels_[j].panel->id, child(path, "id"), "duplicate panel ID");
      check(std::string_view(panelKindName(panel.kind)) != "unknown", child(path, "kind"),
            "invalid panel kind");
      check(panel.grid.w > 0 && panel.grid.h > 0, child(path, "grid"), "panel size must be nonzero");
      check(static_cast<uint64_t>(panel.grid.x) + panel.grid.w <= page.grid_columns,
            child(path, "grid"), "panel exceeds page grid columns");
      check(panel.refresh_ms <= 86400000, child(path, "refresh_ms"), "invalid refresh interval");
      window(panel.time_window, child(path, "time_window"));
      if (!panel.source_id.empty())
        check(containsSource(panel.source_id), child(path, "source_id"), "unknown source ID");
      if (!panel.metric.empty())
        check(!panel.source_id.empty(), child(path, "source_id"), "metric requires a source");
      if (!panel.source_id.empty())
        check(!panel.metric.empty(), child(path, "metric"), "source query requires a metric");
      check(panel.metric.size() <= 128, child(path, "metric"), "exceeds size limit");
      expression(panel.value_expression, child(path, "value_expression"));
      count(panel.thresholds.size(), limits_.max_thresholds_per_panel, child(path, "thresholds"));
      for (size_t j = 0; j < panel.thresholds.size(); ++j) {
        check(std::isfinite(panel.thresholds[j].value),
              child(path, "thresholds") + "[" + std::to_string(j) + "].value", "must be finite");
        check(std::string_view(severityName(panel.thresholds[j].severity)) != "unknown",
              child(path, "thresholds") + "[" + std::to_string(j) + "].severity", "invalid severity");
      }
      count(panel.linked_panel_ids.size(), limits_.max_links_per_panel, child(path, "linked_panel_ids"));
      for (size_t j = 0; j < panel.linked_panel_ids.size(); ++j) {
        const std::string& link = panel.linked_panel_ids[j];
        check(containsPanel(link), child(path, "linked_panel_ids") + "[" + std::to_string(j) + "]",
              "unknown panel ID");
        check(link != panel.id, child(path, "linked_panel_ids") + "[" + std::to_string(j) + "]",
              "panel cannot link to itself");
        for (size_t k = 0; k < j; ++k)
          check(link != panel.linked_panel_ids[k],
                child(path, "linked_panel_ids") + "[" + std::to_string(j) + "]",
                "duplicate panel link");
      }
      count(panel.actions.size(), limits_.max_actions_per_panel, child(path, "actions"));
      for (size_t j = 0; j < panel.actions.size(); ++j) {
        action(panel.actions[j], child(path, "actions") + "[" + std::to_string(j) + "]");
        for (size_t k = 0; k < j; ++k)
          check(panel.actions[j].id != panel.actions[k].id,
                child(path, "actions") + "[" + std::to_string(j) + "].id", "duplicate action ID");
      }
      check(panel.style.precision <= 8 && panel.style.line_width >= 1 && panel.style.line_width <= 8,
            child(path, "style"), "invalid style values");
      const bool text_kind = panel.kind == PanelKind::Text || panel.kind == PanelKind::Card ||
                             panel.kind == PanelKind::Alert;
      if (!text_kind)
        check(panel.text.content.empty() && !panel.text.multiline && panel.text.max_lines == 1,
              child(path, "text"), "text spec is not valid for this panel kind");
      if (panel.kind != PanelKind::Image)
        check(panel.image.asset_id.empty() && panel.image.alt_text.empty() &&
                  panel.image.fit == ImageFit::Contain,
              child(path, "image"), "image spec is only valid for image panels");
      if (panel.kind != PanelKind::Chart)
        check(panel.chart.kind == ChartKind::Line && !panel.chart.show_points &&
                  !panel.chart.fill && !panel.chart.has_y_min && !panel.chart.has_y_max &&
                  panel.chart.y_min == 0.0 && panel.chart.y_max == 0.0,
              child(path, "chart"), "chart spec is only valid for chart panels");
      if (panel.kind != PanelKind::Table)
        check(panel.columns.empty(), child(path, "columns"),
              "columns are only valid for table panels");
      if (!panel.text.multiline)
        check(panel.text.max_lines == 1, child(path, "text.max_lines"),
              "single-line text must have max_lines=1");
      check(panel.text.max_lines >= 1 && panel.text.max_lines <= 64, child(path, "text.max_lines"),
            "must be in 1..64");
      if (panel.kind == PanelKind::Image) {
        check(isImageAsset(panel.image.asset_id), child(path, "image.asset_id"),
              "asset ID must reference an image asset");
        check(std::string_view(imageFitName(panel.image.fit)) != "unknown", child(path, "image.fit"),
              "invalid image fit");
      }
      if (panel.kind == PanelKind::Button)
        check(!panel.actions.empty(), child(path, "actions"), "button requires at least one action");
      if (panel.kind == PanelKind::Chart || panel.kind == PanelKind::Table) {
        check(!panel.source_id.empty() && !panel.metric.empty(), path,
              "chart/table panels require source_id and metric");
      }
      if (panel.kind == PanelKind::Chart) {
        check(std::string_view(chartKindName(panel.chart.kind)) != "unknown", child(path, "chart.kind"),
              "invalid chart kind");
        check(!panel.chart.has_y_min || std::isfinite(panel.chart.y_min), child(path, "chart.y_min"),
              "must be finite");
        check(!panel.chart.has_y_max || std::isfinite(panel.chart.y_max), child(path, "chart.y_max"),
              "must be finite");
        if (panel.chart.has_y_min && panel.chart.has_y_max)
          check(panel.chart.y_min < panel.chart.y_max, child(path, "chart"), "y_min must be below y_max");
      }
      if (panel.kind == PanelKind::Table) {
        count(panel.columns.size(), limits_.max_columns_per_panel, child(path, "columns"));
        check(!panel.columns.empty(), child(path, "columns"), "table requires columns");
        for (size_t j = 0; j < panel.columns.size(); ++j) {
          check(!panel.columns[j].key.empty(), child(path, "columns") + "[" + std::to_string(j) + "].key",
                "column key is required");
          check(panel.columns[j].width >= 1 && panel.columns[j].width <= 64,
                child(path, "columns") + "[" + std::to_string(j) + "].width", "invalid width");
          for (size_t k = 0; k < j; ++k)
            check(panel.columns[j].key != panel.columns[k].key,
                  child(path, "columns") + "[" + std::to_string(j) + "].key", "duplicate column key");
        }
      }
    }
  }

  void collectAssets() {
    uniqueIds(dashboard_.assets, "assets");
    for (size_t i = 0; i < dashboard_.assets.size(); ++i) {
      const Asset& asset = dashboard_.assets[i];
      const std::string path = "assets[" + std::to_string(i) + "]";
      check(std::string_view(assetKindName(asset.kind)) != "unknown", child(path, "kind"),
            "invalid asset kind");
      check(validAssetUri(asset.uri), child(path, "uri"), "expected HTTP(S) or traversal-free sd:/ URI");
      check(validSha256(asset.sha256), child(path, "sha256"), "must be empty or 64 hexadecimal characters");
      check(asset.size_bytes <= 64 * 1024 * 1024, child(path, "size_bytes"), "asset exceeds 64 MiB");
      check(asset.cache_seconds <= 31536000, child(path, "cache_seconds"), "invalid cache lifetime");
    }
  }

  void collectAlertRules() {
    uniqueIds(dashboard_.alert_rules, "alert_rules");
    for (size_t i = 0; i < dashboard_.alert_rules.size(); ++i) {
      const AlertRule& rule = dashboard_.alert_rules[i];
      const std::string path = "alert_rules[" + std::to_string(i) + "]";
      check(containsSource(rule.source_id), child(path, "source_id"), "unknown source ID");
      check(!rule.metric.empty() && rule.metric.size() <= 128, child(path, "metric"), "metric is required");
      check(!rule.expression.empty(), child(path, "expression"), "expression is required");
      expression(rule.expression, child(path, "expression"));
      check(rule.for_ms <= 604800000, child(path, "for_ms"), "invalid alert duration");
      check(rule.style.surfaces != 0 && (rule.style.surfaces & ~kAlertSurfaceAll) == 0,
            child(path, "style.surfaces"), "select at least one known alert surface");
      check(std::string_view(severityName(rule.style.severity)) != "unknown",
            child(path, "style.severity"), "invalid severity");
      if (!rule.style.linked_panel_id.empty())
        check(containsPanel(rule.style.linked_panel_id), child(path, "style.linked_panel_id"),
              "unknown panel ID");
    }
  }

  void collectSamples() {
    for (size_t i = 0; i < dashboard_.samples.size(); ++i) {
      const MetricSample& sample = dashboard_.samples[i];
      const std::string path = "samples[" + std::to_string(i) + "]";
      if (!sample.source_id.empty())
        check(containsSource(sample.source_id), child(path, "source_id"), "unknown source ID");
      check(!sample.metric.empty() && sample.metric.size() <= 128, child(path, "metric"),
            "metric is required and bounded");
      check(sample.timestamp_ms >= 0, child(path, "timestamp_ms"), "must be non-negative");
      check(std::isfinite(sample.value), child(path, "value"), "must be finite");
      labels(sample.labels, child(path, "labels"));
    }
  }

  void collectEvents() {
    uniqueIds(dashboard_.events, "events");
    for (size_t i = 0; i < dashboard_.events.size(); ++i) {
      const Event& event = dashboard_.events[i];
      const std::string path = "events[" + std::to_string(i) + "]";
      if (!event.rule_id.empty())
        check(containsAlert(event.rule_id), child(path, "rule_id"), "unknown alert rule ID");
      if (!event.source_id.empty())
        check(containsSource(event.source_id), child(path, "source_id"), "unknown source ID");
      if (!event.linked_panel_id.empty())
        check(containsPanel(event.linked_panel_id), child(path, "linked_panel_id"), "unknown panel ID");
      check(event.timestamp_ms >= 0, child(path, "timestamp_ms"), "must be non-negative");
      check(std::string_view(severityName(event.severity)) != "unknown", child(path, "severity"),
            "invalid severity");
      check(std::string_view(eventStateName(event.state)) != "unknown", child(path, "state"),
            "invalid event state");
    }
  }

  void validateTemplate(const std::string& text, const std::string& path,
                        const VariableValues& values) {
    std::string output;
    std::string error;
    if (!interpolateVariables(text, values, output, error)) check(false, path, error);
  }

  void resolveTemplates() {
    VariableValues values;
    std::string error;
    if (!defaultVariableValues(dashboard_.variables, {}, values, error)) {
      check(false, "variables", error);
      return;
    }
    for (size_t i = 0; i < dashboard_.sources.size(); ++i) {
      const Source& source = dashboard_.sources[i];
      const std::string path = "sources[" + std::to_string(i) + "].url";
      std::string expanded;
      if (!interpolateVariables(source.url, values, expanded, error)) {
        check(false, path, error);
      } else if (source.kind == SourceKind::Http) {
        check(validHttpUrl(expanded), path, "URL is invalid after variable interpolation");
      }
    }
    for (const auto& location : panels_) {
      validateTemplate(location.panel->text.content, child(location.path, "text.content"), values);
      for (size_t i = 0; i < location.panel->actions.size(); ++i) {
        const Action& action = location.panel->actions[i];
        const std::string path = child(location.path, "actions") + "[" + std::to_string(i) + "]";
        validateTemplate(action.target, child(path, "target"), values);
        validateTemplate(action.value, child(path, "value"), values);
      }
    }
  }

  const Dashboard& dashboard_;
  const Limits& limits_;
  std::vector<ValidationError>& errors_;
  std::vector<PanelLocation> panels_;
  const std::vector<Source>& sources_ = dashboard_.sources;
};

}  // namespace

bool validateDashboardSemantics(const Dashboard& dashboard, const Limits& limits,
                                std::vector<ValidationError>& errors) {
  Validator validator(dashboard, limits, errors);
  validator.run();
  return errors.empty();
}

bool validateDashboard(const Dashboard& dashboard, const Limits& limits,
                       std::vector<ValidationError>& errors) {
  ParseOptions options;
  options.limits = limits;
  Dashboard normalized;
  parseCanonicalFields(dashboardToJson(dashboard), normalized, options, errors);
  return validateDashboardSemantics(dashboard, limits, errors);
}

bool exportDashboard(const Dashboard& dashboard, std::string& out,
                     std::vector<ValidationError>& errors, const Limits& limits) {
  out.clear();
  errors.clear();
  ValidationError limit_error;
  if (!validateLimits(limits, limit_error)) {
    errors.push_back(std::move(limit_error));
    return false;
  }
  if (!validateDashboard(dashboard, limits, errors)) return false;
  out = dashboardToJson(dashboard).toString();
  if (out.size() > limits.max_json_bytes) {
    out.clear();
    errors.push_back({"", "exported configuration exceeds max_json_bytes"});
    return false;
  }
  return true;
}

}  // namespace eego::dashboard
