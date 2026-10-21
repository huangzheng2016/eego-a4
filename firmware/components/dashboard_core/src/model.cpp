// SPDX-License-Identifier: GPL-3.0-or-later
#include "dashboard_core/model.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace eego::dashboard {
namespace {

template <typename Enum, size_t N>
const char* enumName(Enum value, const std::pair<Enum, const char*> (&names)[N]) {
  for (const auto& entry : names) {
    if (entry.first == value) return entry.second;
  }
  return "unknown";
}

template <typename Enum, size_t N>
bool enumValue(std::string_view text, Enum& out,
               const std::pair<Enum, const char*> (&names)[N]) {
  for (const auto& entry : names) {
    if (text == entry.second) {
      out = entry.first;
      return true;
    }
  }
  return false;
}

constexpr std::pair<Severity, const char*> kSeverities[] = {
    {Severity::Info, "info"}, {Severity::Warning, "warning"}, {Severity::Critical, "critical"}};
constexpr std::pair<PanelKind, const char*> kPanels[] = {
    {PanelKind::Text, "text"},       {PanelKind::Image, "image"},
    {PanelKind::Card, "card"},       {PanelKind::Table, "table"},
    {PanelKind::Button, "button"},   {PanelKind::Alert, "alert"},
    {PanelKind::Chart, "chart"},
};
constexpr std::pair<SourceKind, const char*> kSources[] = {
    {SourceKind::Http, "http"}, {SourceKind::Static, "static"}};
constexpr std::pair<VariableKind, const char*> kVariables[] = {
    {VariableKind::Text, "text"},         {VariableKind::Constant, "constant"},
    {VariableKind::Query, "query"},       {VariableKind::TimeRange, "time_range"},
    {VariableKind::Locale, "locale"},
};
constexpr std::pair<ActionKind, const char*> kActions[] = {
    {ActionKind::GotoPage, "goto_page"},   {ActionKind::GotoPanel, "goto_panel"},
    {ActionKind::SetVariable, "set_variable"}, {ActionKind::Refresh, "refresh"},
    {ActionKind::Back, "back"},            {ActionKind::AckAlert, "ack_alert"},
};
constexpr std::pair<ChartKind, const char*> kCharts[] = {
    {ChartKind::Line, "line"}, {ChartKind::Bar, "bar"}, {ChartKind::Sparkline, "sparkline"}};
constexpr std::pair<ImageFit, const char*> kImageFits[] = {
    {ImageFit::Contain, "contain"}, {ImageFit::Cover, "cover"}, {ImageFit::Stretch, "stretch"}};
constexpr std::pair<AssetKind, const char*> kAssets[] = {
    {AssetKind::Image, "image"}, {AssetKind::Font, "font"}, {AssetKind::Binary, "binary"}};
constexpr std::pair<EventState, const char*> kEventStates[] = {
    {EventState::Active, "active"}, {EventState::Cleared, "cleared"}};

bool validVariableName(std::string_view name) {
  if (name.empty() || !((name[0] >= 'A' && name[0] <= 'Z') ||
                        (name[0] >= 'a' && name[0] <= 'z') || name[0] == '_'))
    return false;
  for (const char c : name) {
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '_'))
      return false;
  }
  return true;
}

bool appendLimited(std::string& out, std::string_view part, size_t limit) {
  if (part.size() > limit || out.size() > limit - part.size()) return false;
  out.append(part);
  return true;
}

struct UnitInfo {
  const char* name;
  const char* dimension;
  double factor;
};

constexpr UnitInfo kUnits[] = {
    {"ratio", "ratio", 1.0},       {"percent", "ratio", 0.01},
    {"%", "ratio", 0.01},          {"B", "bytes", 1.0},
    {"KB", "bytes", 1e3},          {"MB", "bytes", 1e6},
    {"GB", "bytes", 1e9},          {"TB", "bytes", 1e12},
    {"KiB", "bytes", 1024.0},      {"MiB", "bytes", 1024.0 * 1024.0},
    {"GiB", "bytes", 1024.0 * 1024.0 * 1024.0},
    {"TiB", "bytes", 1024.0 * 1024.0 * 1024.0 * 1024.0},
    {"ms", "time", 1.0},           {"s", "time", 1000.0},
    {"min", "time", 60000.0},      {"h", "time", 3600000.0},
    {"d", "time", 86400000.0},     {"mm", "distance", 0.001},
    {"cm", "distance", 0.01},      {"m", "distance", 1.0},
    {"km", "distance", 1000.0},
};

const UnitInfo* findUnit(std::string_view unit) {
  for (const auto& info : kUnits) {
    if (unit == info.name) return &info;
  }
  return nullptr;
}

bool isTemperature(std::string_view unit) {
  return unit == "C" || unit == "F" || unit == "K";
}

std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

bool parseDurationMs(std::string_view text, double& milliseconds) {
  static constexpr std::pair<std::string_view, double> kSuffixes[] = {
      {"ms", 1.0}, {"s", 1000.0}, {"m", 60000.0},
      {"h", 3600000.0}, {"d", 86400000.0}, {"w", 604800000.0}};
  if (text.empty() || text.size() > 32) return false;
  size_t suffix_length = 0;
  double factor = 0.0;
  for (const auto& suffix : kSuffixes) {
    if (text.size() > suffix.first.size() &&
        text.substr(text.size() - suffix.first.size()) == suffix.first) {
      suffix_length = suffix.first.size();
      factor = suffix.second;
      break;
    }
  }
  if (suffix_length == 0) return false;
  const std::string number(text.substr(0, text.size() - suffix_length));
  if (number.empty()) return false;
  errno = 0;
  char* end = nullptr;
  const double value = std::strtod(number.c_str(), &end);
  if (errno != 0 || end != number.c_str() + number.size() || !std::isfinite(value) || value < 0.0)
    return false;
  milliseconds = value * factor;
  return std::isfinite(milliseconds);
}

}  // namespace

const char* severityName(Severity value) { return enumName(value, kSeverities); }
bool severityFromString(std::string_view text, Severity& out) {
  return enumValue(text, out, kSeverities);
}
const char* panelKindName(PanelKind value) { return enumName(value, kPanels); }
bool panelKindFromString(std::string_view text, PanelKind& out) {
  return enumValue(text, out, kPanels);
}
const char* sourceKindName(SourceKind value) { return enumName(value, kSources); }
bool sourceKindFromString(std::string_view text, SourceKind& out) {
  return enumValue(text, out, kSources);
}
const char* variableKindName(VariableKind value) { return enumName(value, kVariables); }
bool variableKindFromString(std::string_view text, VariableKind& out) {
  return enumValue(text, out, kVariables);
}
const char* actionKindName(ActionKind value) { return enumName(value, kActions); }
bool actionKindFromString(std::string_view text, ActionKind& out) {
  return enumValue(text, out, kActions);
}
const char* chartKindName(ChartKind value) { return enumName(value, kCharts); }
bool chartKindFromString(std::string_view text, ChartKind& out) {
  return enumValue(text, out, kCharts);
}
const char* imageFitName(ImageFit value) { return enumName(value, kImageFits); }
bool imageFitFromString(std::string_view text, ImageFit& out) {
  return enumValue(text, out, kImageFits);
}
const char* assetKindName(AssetKind value) { return enumName(value, kAssets); }
bool assetKindFromString(std::string_view text, AssetKind& out) {
  return enumValue(text, out, kAssets);
}
const char* eventStateName(EventState value) { return enumName(value, kEventStates); }
bool eventStateFromString(std::string_view text, EventState& out) {
  return enumValue(text, out, kEventStates);
}

bool alertSurfaceFromString(std::string_view text, uint8_t& bit) {
  if (text == "banner") bit = kAlertSurfaceBanner;
  else if (text == "card") bit = kAlertSurfaceCard;
  else if (text == "table_row") bit = kAlertSurfaceTableRow;
  else if (text == "sparkline") bit = kAlertSurfaceSparkline;
  else return false;
  return true;
}

const char* alertSurfaceName(uint8_t bit) {
  switch (bit) {
    case kAlertSurfaceBanner: return "banner";
    case kAlertSurfaceCard: return "card";
    case kAlertSurfaceTableRow: return "table_row";
    case kAlertSurfaceSparkline: return "sparkline";
    default: return "unknown";
  }
}

const std::string* findVariableValue(const VariableValues& values, std::string_view name) {
  for (const auto& value : values) {
    if (value.first == name) return &value.second;
  }
  return nullptr;
}

bool defaultVariableValues(const std::vector<Variable>& variables,
                           const VariableValues& overrides, VariableValues& out,
                           std::string& error) {
  out.clear();
  for (const auto& variable : variables) {
    if (!validVariableName(variable.name)) {
      error = "invalid variable name: " + variable.name;
      return false;
    }
    if (findVariableValue(out, variable.name) != nullptr) {
      error = "duplicate variable: " + variable.name;
      return false;
    }
    out.emplace_back(variable.name, variable.default_value);
  }
  for (size_t i = 0; i < overrides.size(); ++i) {
    for (size_t j = 0; j < i; ++j) {
      if (overrides[i].first == overrides[j].first) {
        error = "duplicate variable override: " + overrides[i].first;
        return false;
      }
    }
  }
  for (const auto& override_value : overrides) {
    auto current = std::find_if(out.begin(), out.end(), [&](const auto& value) {
      return value.first == override_value.first;
    });
    if (current == out.end()) {
      error = "unknown variable override: " + override_value.first;
      return false;
    }
    if (override_value.second.size() > kMaxVariableValueBytes) {
      error = "variable value too long: " + override_value.first;
      return false;
    }
    current->second = override_value.second;
  }
  for (size_t i = 0; i < variables.size(); ++i) {
    const auto& variable = variables[i];
    const std::string& value = out[i].second;
    if (value.size() > kMaxVariableValueBytes) {
      error = "variable value too long: " + variable.name;
      return false;
    }
    if (!variable.options.empty()) {
      bool found = variable.include_all && value == "__all__";
      for (const auto& option : variable.options) found = found || option.value == value;
      if (!found && variable.kind != VariableKind::Query) {
        error = "value is not an option for variable: " + variable.name;
        return false;
      }
    }
  }
  return true;
}

bool interpolateVariables(std::string_view input, const VariableValues& values,
                          std::string& out, std::string& error, size_t max_output_bytes) {
  out.clear();
  size_t position = 0;
  while (position < input.size()) {
    const bool dollar = input[position] == '$';
    const bool escaped = dollar && position + 2 < input.size() && input[position + 1] == '$' &&
                         input[position + 2] == '{';
    if (dollar && !escaped && position + 1 < input.size() && input[position + 1] == '{') {
      const size_t end = input.find('}', position + 2);
      if (end == std::string_view::npos) {
        error = "unterminated variable placeholder";
        return false;
      }
      const std::string_view name = input.substr(position + 2, end - position - 2);
      if (!validVariableName(name)) {
        error = "invalid variable placeholder";
        return false;
      }
      const std::string* value = findVariableValue(values, name);
      if (value == nullptr) {
        error = "undefined variable: " + std::string(name);
        return false;
      }
      if (!appendLimited(out, *value, max_output_bytes)) {
        error = "interpolated output exceeds limit";
        return false;
      }
      position = end + 1;
      continue;
    }
    if (escaped) {
      const size_t end = input.find('}', position + 3);
      if (end == std::string_view::npos) {
        error = "unterminated escaped variable placeholder";
        return false;
      }
      const std::string_view name = input.substr(position + 3, end - position - 3);
      if (!validVariableName(name)) {
        error = "invalid escaped variable placeholder";
        return false;
      }
      if (!appendLimited(out, "${", max_output_bytes) ||
          !appendLimited(out, name, max_output_bytes) ||
          !appendLimited(out, "}", max_output_bytes)) {
        error = "interpolated output exceeds limit";
        return false;
      }
      position = end + 1;
      continue;
    }
    if (!appendLimited(out, input.substr(position, 1), max_output_bytes)) {
      error = "interpolated output exceeds limit";
      return false;
    }
    ++position;
  }
  return true;
}

const char* unitDimension(std::string_view unit) {
  if (isTemperature(unit)) return "temperature";
  const UnitInfo* info = findUnit(unit);
  return info == nullptr ? nullptr : info->dimension;
}

bool convertUnit(double value, std::string_view from, std::string_view to, double& out) {
  if (!std::isfinite(value)) return false;
  if (from == to) {
    out = value;
    return true;
  }
  if (isTemperature(from) || isTemperature(to)) {
    if (!isTemperature(from) || !isTemperature(to)) return false;
    double celsius = 0.0;
    if (from == "C") celsius = value;
    else if (from == "F") celsius = (value - 32.0) * 5.0 / 9.0;
    else celsius = value - 273.15;
    if (to == "C") out = celsius;
    else if (to == "F") out = celsius * 9.0 / 5.0 + 32.0;
    else out = celsius + 273.15;
    return std::isfinite(out);
  }
  const UnitInfo* source = findUnit(from);
  const UnitInfo* target = findUnit(to);
  if (source == nullptr || target == nullptr || std::string_view(source->dimension) != target->dimension)
    return false;
  out = value * source->factor / target->factor;
  return std::isfinite(out);
}

bool parseTimeExpression(std::string_view input, int64_t now_ms, int64_t& out) {
  const std::string_view text = trim(input);
  if (text == "now") {
    out = now_ms;
    return true;
  }
  std::string_view duration;
  if (text.substr(0, 4) == "now-") {
    duration = text.substr(4);
  } else if (!text.empty() && text.front() == '-') {
    duration = text.substr(1);
  } else {
    duration = text;
  }
  double milliseconds = 0.0;
  if (!parseDurationMs(duration, milliseconds) ||
      milliseconds > static_cast<double>(std::numeric_limits<int64_t>::max()))
    return false;
  const int64_t offset = static_cast<int64_t>(milliseconds);
  if (now_ms < std::numeric_limits<int64_t>::min() + offset) return false;
  out = now_ms - offset;
  return true;
}

}  // namespace eego::dashboard
