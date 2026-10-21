// SPDX-License-Identifier: GPL-3.0-or-later
#include "migration.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>

namespace eego::dashboard {
namespace {

void fail(std::vector<ValidationError>& errors, const std::string& field,
          const std::string& message) {
  errors.push_back({field, message});
}

void rename(JsonValue& object, const char* old_name, const char* new_name,
            const std::string& path, std::vector<ValidationError>& errors) {
  JsonValue* old_value = object.find(old_name);
  if (old_value == nullptr) return;
  if (object.find(new_name) != nullptr) {
    fail(errors, path + "." + old_name, std::string("conflicts with ") + new_name);
    return;
  }
  for (auto& member : object.objectItems()) {
    if (member.first == old_name) {
      member.first = new_name;
      return;
    }
  }
}

JsonValue take(JsonValue& object, const char* key) {
  auto& members = object.objectItems();
  for (auto it = members.begin(); it != members.end(); ++it) {
    if (it->first == key) {
      JsonValue value = std::move(it->second);
      members.erase(it);
      return value;
    }
  }
  return JsonValue();
}

void set(JsonValue& object, const char* key, JsonValue value) {
  if (JsonValue* current = object.find(key)) {
    *current = std::move(value);
  } else {
    object.objectItems().emplace_back(key, std::move(value));
  }
}

bool legacyMilliseconds(JsonValue& value) {
  if (value.type() == JsonValue::Type::Int) return value.intValue() >= 0;
  if (value.type() != JsonValue::Type::String) return false;
  const std::string& text = value.stringValue();
  static constexpr std::pair<const char*, double> kSuffixes[] = {
      {"ms", 1.0}, {"s", 1000.0}, {"m", 60000.0}, {"h", 3600000.0}};
  for (const auto& suffix : kSuffixes) {
    const std::string suffix_text(suffix.first);
    if (text.size() <= suffix_text.size() ||
        text.compare(text.size() - suffix_text.size(), suffix_text.size(), suffix_text) != 0)
      continue;
    const std::string number = text.substr(0, text.size() - suffix_text.size());
    errno = 0;
    char* end = nullptr;
    const double parsed = std::strtod(number.c_str(), &end);
    if (errno == 0 && end == number.c_str() + number.size() && std::isfinite(parsed) &&
        parsed >= 0.0 && parsed * suffix.second <= 86400000.0) {
      value = JsonValue::makeInt(static_cast<int64_t>(parsed * suffix.second));
      return true;
    }
    return false;
  }
  return false;
}

void migrateActions(JsonValue& panel, const std::string& path,
                    std::vector<ValidationError>& errors) {
  JsonValue* actions = panel.find("actions");
  if (actions == nullptr || actions->type() != JsonValue::Type::Array) return;
  for (size_t i = 0; i < actions->arrayItems().size(); ++i) {
    JsonValue* action = &actions->arrayItems()[i];
    if (action->type() != JsonValue::Type::Object) continue;
    const std::string action_path = path + ".actions[" + std::to_string(i) + "]";
    rename(*action, "type", "kind", action_path, errors);
    rename(*action, "page", "target", action_path, errors);
    if (JsonValue* kind = action->find("kind");
        kind != nullptr && kind->type() == JsonValue::Type::String && kind->stringValue() == "navigate")
      *kind = JsonValue::makeString("goto_page");
  }
}

void migratePanel(JsonValue& panel, const std::string& path,
                  std::vector<ValidationError>& errors) {
  if (panel.type() != JsonValue::Type::Object) return;
  rename(panel, "panel_type", "kind", path, errors);
  rename(panel, "type", "kind", path, errors);
  if (JsonValue* kind = panel.find("kind"); kind != nullptr && kind->type() == JsonValue::Type::String) {
    if (kind->stringValue() == "graph") *kind = JsonValue::makeString("chart");
    if (kind->stringValue() == "single_text" || kind->stringValue() == "multi_text") {
      const bool multiline = kind->stringValue() == "multi_text";
      *kind = JsonValue::makeString("text");
      if (panel.find("text") == nullptr) {
        JsonValue::Object text;
        text.emplace_back("content", JsonValue::makeString(""));
        text.emplace_back("multiline", JsonValue::makeBool(multiline));
        panel.objectItems().emplace_back("text", JsonValue::makeObject(std::move(text)));
      }
    }
  }

  if (JsonValue* datasource = panel.find("datasource")) {
    if (datasource->type() == JsonValue::Type::String) {
      rename(panel, "datasource", "source_id", path, errors);
    } else if (datasource->type() == JsonValue::Type::Object) {
      const JsonValue datasource_object = *datasource;
      const JsonValue* id = datasource_object.find("id");
      if (id == nullptr) id = datasource_object.find("source_id");
      if (id != nullptr && id->type() == JsonValue::Type::String && panel.find("source_id") == nullptr)
        panel.objectItems().emplace_back("source_id", *id);
      const JsonValue* metric = datasource_object.find("metric");
      if (metric != nullptr && metric->type() == JsonValue::Type::String && panel.find("metric") == nullptr)
        panel.objectItems().emplace_back("metric", *metric);
      JsonValue removed = take(panel, "datasource");
      (void)removed;
    } else {
      fail(errors, path + ".datasource", "legacy datasource must be a string or object");
    }
  }
  if (JsonValue* query = panel.find("query")) {
    if (query->type() == JsonValue::Type::String) {
      rename(panel, "query", "metric", path, errors);
    } else if (query->type() == JsonValue::Type::Object) {
      const JsonValue query_object = *query;
      if (const JsonValue* source = query_object.find("source_id");
          source != nullptr && source->type() == JsonValue::Type::String && panel.find("source_id") == nullptr)
        panel.objectItems().emplace_back("source_id", *source);
      if (const JsonValue* metric = query_object.find("metric");
          metric != nullptr && metric->type() == JsonValue::Type::String && panel.find("metric") == nullptr)
        panel.objectItems().emplace_back("metric", *metric);
      if (const JsonValue* expression = query_object.find("expression");
          expression != nullptr && expression->type() == JsonValue::Type::String &&
          panel.find("value_expression") == nullptr)
        panel.objectItems().emplace_back("value_expression", *expression);
      JsonValue removed = take(panel, "query");
      (void)removed;
    } else {
      fail(errors, path + ".query", "legacy query must be a string or object");
    }
  }
  rename(panel, "refresh", "refresh_ms", path, errors);
  rename(panel, "refresh_interval", "refresh_ms", path, errors);
  if (JsonValue* refresh = panel.find("refresh_ms");
      refresh != nullptr && !legacyMilliseconds(*refresh))
    fail(errors, path + ".refresh_ms", "invalid legacy refresh interval");

  rename(panel, "timeRange", "time_window", path, errors);
  rename(panel, "time_range", "time_window", path, errors);
  if (JsonValue* window = panel.find("time_window")) {
    if (window->type() == JsonValue::Type::String) {
      JsonValue::Object object;
      object.emplace_back("from", *window);
      object.emplace_back("to", JsonValue::makeString("now"));
      *window = JsonValue::makeObject(std::move(object));
    } else if (window->type() == JsonValue::Type::Object) {
      rename(*window, "start", "from", path + ".time_window", errors);
      rename(*window, "end", "to", path + ".time_window", errors);
    }
  }
  if (JsonValue* position = panel.find("position")) {
    if (panel.find("grid") != nullptr) {
      fail(errors, path + ".position", "conflicts with grid");
    } else if (position->type() == JsonValue::Type::Array && position->arrayItems().size() == 4) {
      JsonValue::Object grid;
      const char* names[] = {"x", "y", "w", "h"};
      for (size_t i = 0; i < 4; ++i) grid.emplace_back(names[i], position->arrayItems()[i]);
      JsonValue old = take(panel, "position");
      (void)old;
      panel.objectItems().emplace_back("grid", JsonValue::makeObject(std::move(grid)));
    } else {
      fail(errors, path + ".position", "must be [x,y,w,h]");
    }
  }
  rename(panel, "links", "linked_panel_ids", path, errors);

  if (JsonValue* text = panel.find("text"); text != nullptr && text->type() == JsonValue::Type::String) {
    JsonValue::Object object;
    const bool multiline = text->stringValue().find('\n') != std::string::npos;
    object.emplace_back("content", *text);
    object.emplace_back("multiline", JsonValue::makeBool(multiline));
    *text = JsonValue::makeObject(std::move(object));
  }
  if (JsonValue* image = panel.find("image"); image != nullptr && image->type() == JsonValue::Type::String) {
    JsonValue::Object object;
    object.emplace_back("asset_id", *image);
    *image = JsonValue::makeObject(std::move(object));
  }
  if (JsonValue* thresholds = panel.find("thresholds");
      thresholds != nullptr && thresholds->type() == JsonValue::Type::Array) {
    JsonValue::Array converted;
    for (const auto& threshold : thresholds->arrayItems()) {
      if (threshold.type() == JsonValue::Type::Int || threshold.type() == JsonValue::Type::Double) {
        JsonValue::Object object;
        object.emplace_back("value", threshold);
        object.emplace_back("severity", JsonValue::makeString("info"));
        converted.push_back(JsonValue::makeObject(std::move(object)));
      } else {
        converted.push_back(threshold);
      }
    }
    *thresholds = JsonValue::makeArray(std::move(converted));
  }
  migrateActions(panel, path, errors);
}

void migrateVariables(JsonValue& root, std::vector<ValidationError>& errors) {
  JsonValue* variables = root.find("variables");
  if (variables == nullptr) return;
  if (variables->type() == JsonValue::Type::Object) {
    JsonValue::Array converted;
    for (const auto& member : variables->objectItems()) {
      JsonValue variable;
      if (member.second.isScalar()) {
        JsonValue::Object object;
        object.emplace_back("kind", JsonValue::makeString("text"));
        object.emplace_back("default_value", JsonValue::makeString(member.second.scalarText()));
        variable = JsonValue::makeObject(std::move(object));
      } else if (member.second.type() == JsonValue::Type::Object) {
        variable = member.second;
      } else {
        fail(errors, "variables." + member.first, "legacy variable must be a scalar or object");
        continue;
      }
      rename(variable, "type", "kind", "variables." + member.first, errors);
      rename(variable, "value", "default_value", "variables." + member.first, errors);
      rename(variable, "default", "default_value", "variables." + member.first, errors);
      set(variable, "name", JsonValue::makeString(member.first));
      converted.push_back(std::move(variable));
    }
    *variables = JsonValue::makeArray(std::move(converted));
  }
  if (variables->type() != JsonValue::Type::Array) return;
  for (size_t i = 0; i < variables->arrayItems().size(); ++i) {
    JsonValue* variable = &variables->arrayItems()[i];
    if (variable->type() != JsonValue::Type::Object) continue;
    const std::string path = "variables[" + std::to_string(i) + "]";
    rename(*variable, "type", "kind", path, errors);
    rename(*variable, "value", "default_value", path, errors);
    rename(*variable, "default", "default_value", path, errors);
    if (JsonValue* options = variable->find("options");
        options != nullptr && options->type() == JsonValue::Type::Object) {
      JsonValue::Array converted;
      for (const auto& option : options->objectItems()) {
        if (!option.second.isScalar()) {
          fail(errors, path + ".options." + option.first, "option label must be scalar");
          continue;
        }
        JsonValue::Object object;
        object.emplace_back("label", JsonValue::makeString(option.second.scalarText()));
        object.emplace_back("value", JsonValue::makeString(option.first));
        converted.push_back(JsonValue::makeObject(std::move(object)));
      }
      *options = JsonValue::makeArray(std::move(converted));
    }
  }
}

void migrateAlert(JsonValue& alert, const std::string& path,
                  std::vector<ValidationError>& errors) {
  if (alert.type() != JsonValue::Type::Object) return;
  rename(alert, "condition", "expression", path, errors);
  rename(alert, "for", "for_ms", path, errors);
  if (JsonValue* duration = alert.find("for_ms"); duration != nullptr && !legacyMilliseconds(*duration))
    fail(errors, path + ".for_ms", "invalid legacy alert duration");
  rename(alert, "link", "linked_panel_id", path, errors);

  JsonValue style;
  if (JsonValue* existing = alert.find("style")) {
    if (existing->type() == JsonValue::Type::Object) {
      style = *existing;
    } else if (existing->type() == JsonValue::Type::String) {
      JsonValue::Object object;
      JsonValue::Array surfaces;
      surfaces.push_back(*existing);
      object.emplace_back("surfaces", JsonValue::makeArray(std::move(surfaces)));
      style = JsonValue::makeObject(std::move(object));
    } else if (existing->type() == JsonValue::Type::Array) {
      JsonValue::Object object;
      object.emplace_back("surfaces", *existing);
      style = JsonValue::makeObject(std::move(object));
    } else {
      fail(errors, path + ".style", "invalid legacy alert style");
    }
  }
  if (style.isNull()) style = JsonValue::makeObject({});
  if (JsonValue* severity = alert.find("severity")) {
    if (style.find("severity") == nullptr) style.objectItems().emplace_back("severity", *severity);
    JsonValue removed = take(alert, "severity");
    (void)removed;
  }
  if (JsonValue* link = alert.find("linked_panel_id")) {
    if (style.find("linked_panel_id") == nullptr)
      style.objectItems().emplace_back("linked_panel_id", *link);
    JsonValue removed = take(alert, "linked_panel_id");
    (void)removed;
  }
  rename(style, "link", "linked_panel_id", path + ".style", errors);
  set(alert, "style", std::move(style));
}

}  // namespace

bool migrateV1ToV2(const JsonValue& input, JsonValue& output,
                   std::vector<ValidationError>& errors) {
  if (input.type() != JsonValue::Type::Object) {
    fail(errors, "", "v1 dashboard must be an object");
    return false;
  }
  output = input;
  rename(output, "dashboard_id", "id", "", errors);
  rename(output, "name", "title", "", errors);
  rename(output, "datasources", "sources", "", errors);
  rename(output, "alerts", "alert_rules", "", errors);
  rename(output, "metrics", "samples", "", errors);

  if (JsonValue* panels = output.find("panels")) {
    if (output.find("pages") != nullptr) {
      fail(errors, "panels", "legacy root panels conflicts with pages");
    } else {
      JsonValue::Object page;
      page.emplace_back("id", JsonValue::makeString("main"));
      page.emplace_back("title", JsonValue::makeString("Main"));
      page.emplace_back("panels", *panels);
      JsonValue::Array pages;
      pages.push_back(JsonValue::makeObject(std::move(page)));
      JsonValue removed = take(output, "panels");
      (void)removed;
      output.objectItems().emplace_back("pages", JsonValue::makeArray(std::move(pages)));
      if (output.find("default_page_id") == nullptr)
        output.objectItems().emplace_back("default_page_id", JsonValue::makeString("main"));
    }
  }

  migrateVariables(output, errors);

  if (JsonValue* sources = output.find("sources"); sources != nullptr && sources->type() == JsonValue::Type::Array) {
    for (size_t i = 0; i < sources->arrayItems().size(); ++i) {
      JsonValue* source = &sources->arrayItems()[i];
      if (source->type() != JsonValue::Type::Object) continue;
      const std::string path = "sources[" + std::to_string(i) + "]";
      rename(*source, "type", "kind", path, errors);
      rename(*source, "refresh", "refresh_ms", path, errors);
      rename(*source, "interval_ms", "refresh_ms", path, errors);
      if (JsonValue* refresh = source->find("refresh_ms"); refresh != nullptr && !legacyMilliseconds(*refresh))
        fail(errors, path + ".refresh_ms", "invalid legacy source refresh interval");
      if (JsonValue* kind = source->find("kind"); kind != nullptr && kind->type() == JsonValue::Type::String &&
          (kind->stringValue() == "http_poll" || kind->stringValue() == "http_json"))
        *kind = JsonValue::makeString("http");
    }
  }

  if (JsonValue* pages = output.find("pages"); pages != nullptr && pages->type() == JsonValue::Type::Array) {
    for (size_t i = 0; i < pages->arrayItems().size(); ++i) {
      JsonValue* page = &pages->arrayItems()[i];
      if (page->type() != JsonValue::Type::Object) continue;
      const std::string page_path = "pages[" + std::to_string(i) + "]";
      rename(*page, "widgets", "panels", page_path, errors);
      JsonValue* panels = page->find("panels");
      if (panels == nullptr || panels->type() != JsonValue::Type::Array) continue;
      for (size_t j = 0; j < panels->arrayItems().size(); ++j) {
        JsonValue* panel = &panels->arrayItems()[j];
        migratePanel(*panel, page_path + ".panels[" + std::to_string(j) + "]", errors);
      }
    }
  }

  if (JsonValue* alerts = output.find("alert_rules"); alerts != nullptr && alerts->type() == JsonValue::Type::Array) {
    for (size_t i = 0; i < alerts->arrayItems().size(); ++i) {
      JsonValue* alert = &alerts->arrayItems()[i];
      migrateAlert(*alert, "alert_rules[" + std::to_string(i) + "]", errors);
    }
  }

  if (JsonValue* assets = output.find("assets"); assets != nullptr && assets->type() == JsonValue::Type::Array) {
    for (size_t i = 0; i < assets->arrayItems().size(); ++i) {
      JsonValue* asset = &assets->arrayItems()[i];
      if (asset->type() != JsonValue::Type::Object) continue;
      const std::string path = "assets[" + std::to_string(i) + "]";
      rename(*asset, "type", "kind", path, errors);
      rename(*asset, "url", "uri", path, errors);
      rename(*asset, "bytes", "size_bytes", path, errors);
    }
  }

  if (JsonValue* samples = output.find("samples"); samples != nullptr && samples->type() == JsonValue::Type::Array) {
    for (size_t i = 0; i < samples->arrayItems().size(); ++i) {
      JsonValue* sample = &samples->arrayItems()[i];
      if (sample->type() == JsonValue::Type::Object)
        rename(*sample, "time", "timestamp_ms", "samples[" + std::to_string(i) + "]", errors);
    }
  }

  set(output, "schema", JsonValue::makeString(kSchemaName));
  set(output, "schema_version", JsonValue::makeInt(kSchemaVersion));
  return errors.empty();
}

}  // namespace eego::dashboard
