// SPDX-License-Identifier: GPL-3.0-or-later
// Shared helpers: firmware paths and the corpus of real dashboards (examples,
// expanded presets, the embedded template library and the built-in sample).
#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "dashboard_core/json.h"
#include "test_framework.h"

namespace hosttest {

namespace fs = std::filesystem;
using eego::dashboard::JsonParseError;
using eego::dashboard::JsonValue;

inline fs::path firmwareRoot() { return fs::path(EEGO_FIRMWARE_ROOT); }

inline std::string slurp(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

inline bool parseJson(const std::string& text, JsonValue& out, std::string* error = nullptr) {
  JsonParseError err;
  // Bundles nest one level deeper than a dashboard (bundle -> dashboard).
  const bool ok = eego::dashboard::jsonParse(text, out, err, 24);
  if (!ok && error != nullptr) *error = "byte " + std::to_string(err.offset) + ": " + err.message;
  return ok;
}

inline const JsonValue* member(const JsonValue& object, const char* key) {
  return object.type() == JsonValue::Type::Object ? object.find(key) : nullptr;
}

struct NamedBundle {
  std::string name;
  JsonValue dashboard;     // eego.dashboard/2 object
  JsonValue source_plans;  // array (may be empty)
};

// Mirrors tools/dashboard_validation.py bundle_from_preset().
inline NamedBundle bundleFromPreset(const std::string& name, const JsonValue& preset) {
  NamedBundle out;
  out.name = name;
  const JsonValue* tmpl = member(preset, "dashboard");
  const JsonValue* pages = tmpl ? member(*tmpl, "pages") : nullptr;
  std::string first_page;
  if (pages != nullptr && pages->type() == JsonValue::Type::Array && !pages->arrayItems().empty()) {
    if (const JsonValue* id = member(pages->arrayItems().front(), "id")) first_page = id->stringValue();
  }
  const JsonValue* id = member(preset, "id");
  const JsonValue* title = member(preset, "title");
  JsonValue::Object dash = {
      {"schema", JsonValue::makeString("eego.dashboard/2")},
      {"schema_version", JsonValue::makeInt(2)},
      {"id", JsonValue::makeString(id ? id->stringValue() : name)},
      {"title", JsonValue::makeString(title ? title->stringValue() : "")},
      {"default_page_id", JsonValue::makeString(first_page)},
      {"refresh_ms", JsonValue::makeInt(60000)},
  };
  for (const char* key : {"sources", "variables", "pages", "alert_rules", "assets", "samples", "events"}) {
    const JsonValue* value = tmpl ? member(*tmpl, key) : nullptr;
    dash.emplace_back(key, value ? *value : JsonValue::makeArray({}));
  }
  out.dashboard = JsonValue::makeObject(std::move(dash));
  const JsonValue* plans = member(preset, "source_plans");
  out.source_plans = plans ? *plans : JsonValue::makeArray({});
  return out;
}

inline bool bundleFromJson(const std::string& name, const JsonValue& bundle, NamedBundle& out) {
  const JsonValue* dash = member(bundle, "dashboard");
  if (dash == nullptr) return false;
  out.name = name;
  out.dashboard = *dash;
  const JsonValue* plans = member(bundle, "source_plans");
  out.source_plans = plans ? *plans : JsonValue::makeArray({});
  return true;
}

// Extracts the raw-string sample bundle from DashboardStore::sampleJson().
inline bool builtinSampleText(std::string& out) {
  const std::string source =
      slurp(firmwareRoot() / "components/admin/src/dashboard_config.cpp");
  const std::string open = "R\"JSON(";
  const std::string close = ")JSON\"";
  const size_t anchor = source.find("sampleJson()");
  if (anchor == std::string::npos) return false;
  const size_t begin = source.find(open, anchor);
  if (begin == std::string::npos) return false;
  const size_t end = source.find(close, begin + open.size());
  if (end == std::string::npos) return false;
  out = source.substr(begin + open.size(), end - begin - open.size());
  return true;
}

inline std::vector<fs::path> jsonFilesIn(const fs::path& dir) {
  std::vector<fs::path> out;
  if (!fs::is_directory(dir)) return out;
  for (const auto& entry : fs::directory_iterator(dir)) {
    if (entry.is_regular_file() && entry.path().extension() == ".json") out.push_back(entry.path());
  }
  std::sort(out.begin(), out.end());
  return out;
}

// Every real dashboard in the repository. Loading problems are test failures.
inline std::vector<NamedBundle> loadCorpus() {
  std::vector<NamedBundle> out;
  const fs::path presets = firmwareRoot() / "presets";
  for (const auto& path : jsonFilesIn(presets / "examples")) {
    JsonValue root;
    std::string error;
    NamedBundle bundle;
    CHECK_MSG(parseJson(slurp(path), root, &error), path << ": " << error);
    CHECK_MSG(bundleFromJson("examples/" + path.filename().string(), root, bundle),
              path << ": not a bundle");
    if (!bundle.name.empty()) out.push_back(std::move(bundle));
  }
  for (const char* kind : {"panels", "layouts"}) {
    for (const auto& path : jsonFilesIn(presets / kind)) {
      JsonValue root;
      std::string error;
      CHECK_MSG(parseJson(slurp(path), root, &error), path << ": " << error);
      out.push_back(bundleFromPreset(std::string(kind) + "/" + path.filename().string(), root));
    }
  }
  std::string sample;
  CHECK_MSG(builtinSampleText(sample), "built-in sampleJson() raw string not found");
  if (!sample.empty()) {
    JsonValue root;
    std::string error;
    NamedBundle bundle;
    CHECK_MSG(parseJson(sample, root, &error), "builtin sample: " << error);
    if (bundleFromJson("builtin:sampleJson", root, bundle)) out.push_back(std::move(bundle));
  }
  return out;
}

}  // namespace hosttest
