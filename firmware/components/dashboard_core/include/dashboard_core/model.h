// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eego::dashboard {

inline constexpr char kSchemaName[] = "eego.dashboard/2";
inline constexpr uint32_t kSchemaVersion = 2;

// Schema-facing string maxima are Unicode code points. Document and runtime
// memory budgets remain byte-based and are named explicitly where used.
inline constexpr size_t kMaxIdCodePoints = 64;
inline constexpr size_t kMaxTitleCodePoints = 160;
inline constexpr size_t kMaxTextCodePoints = 4096;
inline constexpr size_t kMaxUrlCodePoints = 1024;
inline constexpr size_t kMaxExpressionCodePoints = 512;
inline constexpr size_t kMaxVariableValueCodePoints = 512;
inline constexpr size_t kMaxLabels = 16;
inline constexpr size_t kMaxHeaders = 16;

enum class Severity : uint8_t { Info = 0, Warning = 1, Critical = 2 };
const char* severityName(Severity severity);
bool severityFromString(std::string_view text, Severity& out);

enum class PanelKind : uint8_t { Text, Image, Card, Table, Button, Alert, Chart };
const char* panelKindName(PanelKind kind);
bool panelKindFromString(std::string_view text, PanelKind& out);

enum class SourceKind : uint8_t { Http, Static };
const char* sourceKindName(SourceKind kind);
bool sourceKindFromString(std::string_view text, SourceKind& out);

enum class VariableKind : uint8_t { Text, Constant, Query, TimeRange, Locale };
const char* variableKindName(VariableKind kind);
bool variableKindFromString(std::string_view text, VariableKind& out);

enum class ActionKind : uint8_t { GotoPage, GotoPanel, SetVariable, Refresh, Back, AckAlert };
const char* actionKindName(ActionKind kind);
bool actionKindFromString(std::string_view text, ActionKind& out);

enum class ChartKind : uint8_t { Line, Bar, Sparkline };
const char* chartKindName(ChartKind kind);
bool chartKindFromString(std::string_view text, ChartKind& out);

enum class ImageFit : uint8_t { Contain, Cover, Stretch };
const char* imageFitName(ImageFit fit);
bool imageFitFromString(std::string_view text, ImageFit& out);

enum class AssetKind : uint8_t { Image, Font, Binary };
const char* assetKindName(AssetKind kind);
bool assetKindFromString(std::string_view text, AssetKind& out);

enum class EventState : uint8_t { Active, Cleared };
const char* eventStateName(EventState state);
bool eventStateFromString(std::string_view text, EventState& out);

struct KeyValue {
  std::string key;
  std::string value;
};

struct GridRect {
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t w = 6;
  uint32_t h = 4;
};

struct TimeWindow {
  std::string from = "-1h";
  std::string to = "now";
};

struct Threshold {
  double value = 0.0;
  Severity severity = Severity::Info;
  std::string label;
};

struct Style {
  std::string unit;
  uint32_t precision = 2;
  uint32_t line_width = 1;
  bool show_legend = true;
  bool invert = false;
};

struct Action {
  std::string id;
  ActionKind kind = ActionKind::Refresh;
  std::string label;
  // refresh: all/current/source ID; ack_alert: current/focused. Other kinds
  // use a registered page, panel, or variable ID as appropriate.
  std::string target;
  std::string value;
  bool confirm = false;
};

struct TextSpec {
  std::string content;
  bool multiline = false;
  uint32_t max_lines = 1;
};

struct ImageSpec {
  std::string asset_id;
  std::string alt_text;
  ImageFit fit = ImageFit::Contain;
};

struct ChartSpec {
  ChartKind kind = ChartKind::Line;
  bool show_points = false;
  bool fill = false;
  bool has_y_min = false;
  bool has_y_max = false;
  double y_min = 0.0;
  double y_max = 0.0;
};

struct TableColumn {
  std::string key;
  std::string title;
  std::string unit;
  uint32_t width = 4;
};

struct PanelSpec {
  std::string id;
  PanelKind kind = PanelKind::Text;
  GridRect grid;
  std::string title;
  bool visible = true;
  std::string source_id;
  std::string metric;
  std::string value_path;
  std::string value_expression;
  TimeWindow time_window;
  uint32_t refresh_ms = 60000;
  std::vector<Threshold> thresholds;
  Style style;
  std::vector<std::string> linked_panel_ids;
  std::vector<Action> actions;
  TextSpec text;
  ImageSpec image;
  ChartSpec chart;
  std::vector<TableColumn> columns;
};

struct Source {
  std::string id;
  SourceKind kind = SourceKind::Http;
  bool enabled = true;
  std::string url;
  uint32_t refresh_ms = 60000;
  uint32_t timeout_ms = 10000;
  uint32_t max_response_bytes = 64 * 1024;
  bool tls_verify = true;
  std::string auth_token_ref;
  std::vector<KeyValue> headers;
};

struct VariableOption {
  std::string label;
  std::string value;
};

struct Variable {
  std::string name;
  std::string label;
  VariableKind kind = VariableKind::Text;
  std::string default_value;
  std::vector<VariableOption> options;
  std::string source_id;
  std::string value_path;
  bool multi = false;
  bool include_all = false;
};

// Surfaces are bits, not an enum list: one alert may drive any combination.
enum AlertSurface : uint8_t {
  kAlertSurfaceBanner = 1U << 0,
  kAlertSurfaceCard = 1U << 1,
  kAlertSurfaceTableRow = 1U << 2,
  kAlertSurfaceSparkline = 1U << 3,
};
inline constexpr uint8_t kAlertSurfaceAll = kAlertSurfaceBanner | kAlertSurfaceCard |
                                             kAlertSurfaceTableRow | kAlertSurfaceSparkline;
bool alertSurfaceFromString(std::string_view text, uint8_t& bit);
const char* alertSurfaceName(uint8_t bit);

struct AlertStyle {
  uint8_t surfaces = kAlertSurfaceCard;
  Severity severity = Severity::Info;
  std::string linked_panel_id;
};

struct AlertRule {
  std::string id;
  std::string title;
  bool enabled = true;
  std::string source_id;
  std::string metric;
  std::string expression;
  uint32_t for_ms = 0;
  std::string message;
  AlertStyle style;
};

struct MetricSample {
  std::string source_id;
  std::string metric;
  int64_t timestamp_ms = 0;
  double value = 0.0;
  std::string unit;
  std::vector<KeyValue> labels;
};

struct Event {
  std::string id;
  std::string rule_id;
  std::string source_id;
  int64_t timestamp_ms = 0;
  Severity severity = Severity::Info;
  EventState state = EventState::Active;
  std::string title;
  std::string message;
  std::string linked_panel_id;
};

struct Asset {
  std::string id;
  AssetKind kind = AssetKind::Image;
  std::string uri;
  std::string media_type;
  std::string sha256;
  uint64_t size_bytes = 0;
  uint32_t cache_seconds = 3600;
};

struct Page {
  std::string id;
  std::string title;
  bool visible = true;
  uint32_t grid_columns = 24;
  uint32_t row_height = 16;
  std::vector<PanelSpec> panels;
};

struct Dashboard {
  std::string id;
  std::string title;
  std::string default_page_id;
  uint32_t refresh_ms = 60000;
  std::vector<Source> sources;
  std::vector<Variable> variables;
  std::vector<Page> pages;
  std::vector<AlertRule> alert_rules;
  std::vector<Asset> assets;
  std::vector<MetricSample> samples;
  std::vector<Event> events;
};

using VariableValues = std::vector<std::pair<std::string, std::string>>;

const std::string* findVariableValue(const VariableValues& values, std::string_view name);
bool defaultVariableValues(const std::vector<Variable>& variables,
                           const VariableValues& overrides, VariableValues& out,
                           std::string& error);

// ${name} interpolates one pass only; $${name} emits a literal ${name}.
// Values are never re-scanned, so variable content cannot inject expressions.
// The default byte budget admits the largest UTF-8 form of a maximum-size text field.
bool interpolateVariables(std::string_view input, const VariableValues& values,
                          std::string& out, std::string& error,
                          size_t max_output_bytes = kMaxTextCodePoints * 4);

// Dimensioned, finite conversions. Supported families: temperature (C/F/K),
// ratio (ratio/percent), bytes (B/KB/MB/GB/TB/KiB/MiB/GiB/TiB), time
// (ms/s/min/h/d), and distance (mm/cm/m/km). Units are case-sensitive.
bool convertUnit(double value, std::string_view from, std::string_view to, double& out);
const char* unitDimension(std::string_view unit);

// Time expressions: now, now-15m, -1h, or a duration such as 90s.
bool parseTimeExpression(std::string_view text, int64_t now_ms, int64_t& out);

}  // namespace eego::dashboard
