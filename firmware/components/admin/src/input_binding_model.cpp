// SPDX-License-Identifier: GPL-3.0-or-later
#include "admin/input_binding_model.h"

#include <algorithm>
#include <array>
#include <limits>

namespace eego::admin {

namespace {

using alert_core::JsonValue;
using eego::dashboard::ActionKind;
using eego::dashboard::Dashboard;

// Builtins (device-side semantics, see firmware/README.md「设备操作」):
//   page_previous/page_next   dashboard: previous/next screen; lists
//                             (settings, alert detail): previous/next item
//   focus_previous/focus_next dashboard: previous/next panel focus;
//                             settings: previous/next settings page
//   home      dashboard first screen      settings  settings screen
//   status    device status screen        help      input help screen
//   refresh   refresh dashboard sources (current screen)
constexpr std::array<const char*, 9> kBuiltins = {
    "page_previous", "page_next", "focus_previous", "focus_next", "home",
    "settings",      "status",    "help",           "refresh",
};
constexpr std::array<const char*, 7> kLocalPages = {
    "dashboard", "onboarding", "detail", "status", "settings", "help", "*",
};

struct TriggerName {
  InputTrigger trigger;
  const char* name;
};
constexpr std::array<TriggerName, 9> kTriggers = {{
    {InputTrigger::UpShort, "up.short"},
    {InputTrigger::UpLong, "up.long"},
    {InputTrigger::UpDouble, "up.double"},
    {InputTrigger::DownShort, "down.short"},
    {InputTrigger::DownLong, "down.long"},
    {InputTrigger::DownDouble, "down.double"},
    {InputTrigger::UpDownChord, "up_down.chord"},
    {InputTrigger::FrontLong, "front.long"},
    {InputTrigger::FrontDouble, "front.double"},
}};

InputBinding builtin(InputTrigger trigger, const char* name) {
  InputBinding out;
  out.trigger = trigger;
  out.action.kind = InputActionKind::Builtin;
  out.action.target = name;
  return out;
}

void addError(InputBindingErrors& errors, const std::string& field,
              const std::string& message) {
  if (errors.size() < 16) errors.push_back({field, message});
}

bool objectFields(const JsonValue& value, std::initializer_list<const char*> allowed,
                  const std::string& path, InputBindingErrors& errors) {
  if (value.type() != JsonValue::Type::Object) {
    addError(errors, path, "object required");
    return false;
  }
  bool ok = true;
  for (const auto& item : value.objectItems()) {
    const bool known = std::find_if(allowed.begin(), allowed.end(), [&](const char* key) {
                         return item.first == key;
                       }) != allowed.end();
    if (!known) {
      addError(errors, path + "." + item.first, "unknown field");
      ok = false;
    }
    size_t count = 0;
    for (const auto& other : value.objectItems()) {
      if (other.first == item.first) ++count;
    }
    if (count != 1) {
      addError(errors, path + "." + item.first, "duplicate field");
      ok = false;
    }
  }
  return ok;
}

bool stringField(const JsonValue& object, const char* name, std::string& out,
                 size_t max_bytes, const std::string& path, InputBindingErrors& errors,
                 bool required = true) {
  const JsonValue* value = object.find(name);
  if (value == nullptr) {
    if (required) addError(errors, path + "." + name, "required");
    return !required;
  }
  if (value->type() != JsonValue::Type::String || value->stringValue().empty() ||
      value->stringValue().size() > max_bytes) {
    addError(errors, path + "." + name,
             "non-empty string up to " + std::to_string(max_bytes) + " bytes required");
    return false;
  }
  out = value->stringValue();
  return true;
}

bool validLocalPage(const std::string& page) {
  return std::find(kLocalPages.begin(), kLocalPages.end(), page) != kLocalPages.end();
}

bool validDashboardPageSyntax(const std::string& page) {
  if (page.rfind("dashboard:", 0) != 0 || page.size() <= 10 || page.size() > 74) return false;
  return page.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.:-",
                                10) == std::string::npos;
}

bool parseAction(const JsonValue& value, InputAction& out, const std::string& path,
                 InputBindingErrors& errors) {
  std::string kind;
  if (!stringField(value, "kind", kind, 16, path, errors)) return false;
  if (kind == "none") {
    if (!objectFields(value, {"kind"}, path, errors)) return false;
    out = InputAction{};
    return true;
  }
  if (kind == "builtin") {
    if (!objectFields(value, {"kind", "target"}, path, errors)) return false;
    out = InputAction{};
    out.kind = InputActionKind::Builtin;
    return stringField(value, "target", out.target, 32, path, errors);
  }
  if (kind == "action") {
    if (!objectFields(value, {"kind", "panel_id", "action_id"}, path, errors)) return false;
    out = InputAction{};
    out.kind = InputActionKind::DashboardAction;
    const bool panel = stringField(value, "panel_id", out.panel_id, 64, path, errors);
    const bool action = stringField(value, "action_id", out.action_id, 64, path, errors);
    return panel && action;
  }
  addError(errors, path + ".kind", "expected none, builtin, or action");
  return false;
}

const dashboard::Action* findAction(const Dashboard& dashboard, const std::string& panel_id,
                                    const std::string& action_id) {
  for (const auto& page : dashboard.pages) {
    for (const auto& panel : page.panels) {
      if (panel.id != panel_id) continue;
      for (const auto& action : panel.actions) {
        if (action.id == action_id) return &action;
      }
    }
  }
  return nullptr;
}

bool hasPage(const Dashboard& dashboard, const std::string& id) {
  return std::find_if(dashboard.pages.begin(), dashboard.pages.end(), [&](const auto& page) {
           return page.id == id;
         }) != dashboard.pages.end();
}

}  // namespace

const char* inputTriggerName(InputTrigger trigger) {
  for (const auto& item : kTriggers) {
    if (item.trigger == trigger) return item.name;
  }
  return "unknown";
}

bool inputTriggerFromString(std::string_view text, InputTrigger& out) {
  for (const auto& item : kTriggers) {
    if (text == item.name) {
      out = item.trigger;
      return true;
    }
  }
  return false;
}

bool operator==(const InputAction& left, const InputAction& right) {
  return left.kind == right.kind && left.target == right.target &&
         left.panel_id == right.panel_id && left.action_id == right.action_id;
}

bool operator==(const InputBinding& left, const InputBinding& right) {
  return left.page == right.page && left.trigger == right.trigger && left.action == right.action;
}

bool inputBuiltinAllowed(std::string_view name) {
  return std::find(kBuiltins.begin(), kBuiltins.end(), name) != kBuiltins.end();
}

const std::vector<InputBinding>& defaultInputBindings() {
  // Up/Down short = previous/next screen (or item in a list), long = panel
  // focus; front long = Settings, front double = Status. Power short/long
  // (select/back) and front short (home) are fixed and not bindable.
  static const std::vector<InputBinding> defaults = {
      builtin(InputTrigger::UpShort, "page_previous"),
      builtin(InputTrigger::DownShort, "page_next"),
      builtin(InputTrigger::UpLong, "focus_previous"),
      builtin(InputTrigger::DownLong, "focus_next"),
      builtin(InputTrigger::FrontLong, "settings"),
      builtin(InputTrigger::FrontDouble, "status"),
  };
  return defaults;
}

bool inputBindingsFromJson(const JsonValue& root, bool require_revision,
                           InputBindingSet& out, InputBindingErrors& errors) {
  errors.clear();
  InputBindingSet candidate;
  if (!objectFields(root, {"schema_version", "revision", "bindings"}, "", errors)) {
    return false;
  }
  const JsonValue* version = root.find("schema_version");
  if (version == nullptr || version->type() != JsonValue::Type::Int ||
      version->intValue() != kInputBindingSchemaVersion) {
    addError(errors, "schema_version", "must be 1");
  }
  const JsonValue* revision = root.find("revision");
  if (revision == nullptr) {
    if (require_revision) addError(errors, "revision", "required");
  } else if (revision->type() != JsonValue::Type::Int || revision->intValue() < 1 ||
             revision->intValue() > std::numeric_limits<uint32_t>::max()) {
    addError(errors, "revision", "positive uint32 required");
  } else {
    candidate.revision = static_cast<uint32_t>(revision->intValue());
  }
  const JsonValue* bindings = root.find("bindings");
  if (bindings == nullptr || bindings->type() != JsonValue::Type::Array) {
    addError(errors, "bindings", "array required");
    return false;
  }
  if (bindings->arrayItems().size() > kMaxInputBindings) {
    addError(errors, "bindings", "at most 24 sparse overrides");
    return false;
  }
  for (size_t i = 0; i < bindings->arrayItems().size(); ++i) {
    const JsonValue& item = bindings->arrayItems()[i];
    const std::string path = "bindings[" + std::to_string(i) + "]";
    if (!objectFields(item, {"page", "trigger", "action"}, path, errors)) continue;
    InputBinding binding;
    std::string trigger;
    const bool page_ok = stringField(item, "page", binding.page, 74, path, errors);
    const bool trigger_ok = stringField(item, "trigger", trigger, 24, path, errors);
    if (trigger_ok && !inputTriggerFromString(trigger, binding.trigger)) {
      addError(errors, path + ".trigger", "reserved or unknown trigger");
    }
    const JsonValue* action = item.find("action");
    if (action == nullptr) {
      addError(errors, path + ".action", "required");
      continue;
    }
    if (page_ok && trigger_ok && parseAction(*action, binding.action, path + ".action", errors)) {
      candidate.bindings.push_back(std::move(binding));
    }
  }
  if (!errors.empty() || !validateInputBindingsSyntax(candidate.bindings, errors)) return false;
  out = std::move(candidate);
  return true;
}

bool parseStoredInputBindings(const std::string& text, InputBindingSet& out, std::string& reason) {
  reason.clear();
  JsonValue root;
  alert_core::JsonParseError parse_error;
  if (!alert_core::jsonParse(text, root, parse_error)) {
    reason = "record is not valid JSON";
    return false;
  }
  InputBindingSet candidate;
  InputBindingErrors errors;
  if (!inputBindingsFromJson(root, true, candidate, errors)) {
    reason = errors.empty() ? std::string("record failed validation")
                            : errors.front().field + ": " + errors.front().message;
    return false;
  }
  candidate.loaded = true;
  out = std::move(candidate);
  return true;
}

JsonValue inputBindingToJson(const InputBinding& binding) {
  JsonValue::Object action;
  switch (binding.action.kind) {
    case InputActionKind::None:
      action.emplace_back("kind", JsonValue::makeString("none"));
      break;
    case InputActionKind::Builtin:
      action.emplace_back("kind", JsonValue::makeString("builtin"));
      action.emplace_back("target", JsonValue::makeString(binding.action.target));
      break;
    case InputActionKind::DashboardAction:
      action.emplace_back("kind", JsonValue::makeString("action"));
      action.emplace_back("panel_id", JsonValue::makeString(binding.action.panel_id));
      action.emplace_back("action_id", JsonValue::makeString(binding.action.action_id));
      break;
  }
  return JsonValue::makeObject({
      {"page", JsonValue::makeString(binding.page)},
      {"trigger", JsonValue::makeString(inputTriggerName(binding.trigger))},
      {"action", JsonValue::makeObject(std::move(action))},
  });
}

JsonValue inputBindingListToJson(const std::vector<InputBinding>& bindings) {
  JsonValue::Array out;
  out.reserve(bindings.size());
  for (const auto& binding : bindings) out.push_back(inputBindingToJson(binding));
  return JsonValue::makeArray(std::move(out));
}

JsonValue inputBindingsToJson(const InputBindingSet& set) {
  return JsonValue::makeObject({
      {"schema_version", JsonValue::makeInt(kInputBindingSchemaVersion)},
      {"revision", JsonValue::makeInt(set.revision)},
      {"bindings", inputBindingListToJson(set.bindings)},
  });
}

bool validateInputBindingsSyntax(const std::vector<InputBinding>& bindings,
                                 InputBindingErrors& errors) {
  errors.clear();
  if (bindings.size() > kMaxInputBindings) {
    addError(errors, "bindings", "at most 24 sparse overrides");
  }
  for (size_t i = 0; i < bindings.size(); ++i) {
    const InputBinding& binding = bindings[i];
    const std::string path = "bindings[" + std::to_string(i) + "]";
    if (!validLocalPage(binding.page) && !validDashboardPageSyntax(binding.page)) {
      addError(errors, path + ".page", "unknown page context");
    }
    for (size_t j = 0; j < i; ++j) {
      if (bindings[j].page == binding.page && bindings[j].trigger == binding.trigger) {
        addError(errors, path, "duplicate page and trigger");
      }
    }
    switch (binding.action.kind) {
      case InputActionKind::None:
        if (!binding.action.target.empty() || !binding.action.panel_id.empty() ||
            !binding.action.action_id.empty()) {
          addError(errors, path + ".action", "none does not accept parameters");
        }
        break;
      case InputActionKind::Builtin:
        if (!inputBuiltinAllowed(binding.action.target) || !binding.action.panel_id.empty() ||
            !binding.action.action_id.empty()) {
          addError(errors, path + ".action.target", "builtin is not whitelisted");
        }
        break;
      case InputActionKind::DashboardAction:
        if (!binding.action.target.empty() || binding.action.panel_id.empty() ||
            binding.action.action_id.empty() || binding.action.panel_id.size() > 64 ||
            binding.action.action_id.size() > 64) {
          addError(errors, path + ".action", "invalid dashboard panel/action reference");
        }
        break;
    }
  }
  return errors.empty();
}

bool validateInputBindings(const std::vector<InputBinding>& bindings,
                           const Dashboard* dashboard, InputBindingErrors& errors) {
  if (!validateInputBindingsSyntax(bindings, errors)) return false;
  for (size_t i = 0; i < bindings.size(); ++i) {
    const InputBinding& binding = bindings[i];
    const std::string path = "bindings[" + std::to_string(i) + "]";
    if (validDashboardPageSyntax(binding.page)) {
      const std::string id = binding.page.substr(10);
      if (dashboard == nullptr || !hasPage(*dashboard, id)) {
        addError(errors, path + ".page", "unregistered dashboard page");
      }
    }
    if (binding.action.kind == InputActionKind::DashboardAction &&
        (dashboard == nullptr ||
         findAction(*dashboard, binding.action.panel_id, binding.action.action_id) == nullptr)) {
      addError(errors, path + ".action", "unregistered dashboard panel/action");
    }
  }
  return errors.empty();
}

bool resolveInputBinding(const std::vector<InputBinding>& overrides,
                         const std::string& page_context, InputTrigger trigger,
                         InputBinding& out) {
  auto find = [&](const std::string& page) -> const InputBinding* {
    for (const auto& binding : overrides) {
      if (binding.page == page && binding.trigger == trigger) return &binding;
    }
    return nullptr;
  };
  const InputBinding* selected = find(page_context);
  if (selected == nullptr && page_context.rfind("dashboard:", 0) == 0) {
    selected = find("dashboard");
  }
  if (selected == nullptr) selected = find("*");
  if (selected != nullptr) {
    out = *selected;
    return true;
  }
  for (const auto& binding : defaultInputBindings()) {
    if (binding.trigger == trigger) {
      out = binding;
      return true;
    }
  }
  return false;
}

JsonValue inputBindingSchema(const Dashboard* dashboard) {
  JsonValue::Array triggers;
  for (const auto& item : kTriggers) triggers.push_back(JsonValue::makeString(item.name));
  JsonValue::Array builtins;
  for (const char* name : kBuiltins) builtins.push_back(JsonValue::makeString(name));
  JsonValue::Array actions;
  if (dashboard != nullptr) {
    for (const auto& page : dashboard->pages) {
      for (const auto& panel : page.panels) {
        for (const auto& action : panel.actions) {
          actions.push_back(JsonValue::makeObject({
              {"panel_id", JsonValue::makeString(panel.id)},
              {"action_id", JsonValue::makeString(action.id)},
              {"action_kind", JsonValue::makeString(dashboard::actionKindName(action.kind))},
              {"target", JsonValue::makeString(action.target)},
              {"confirm", JsonValue::makeBool(action.confirm || action.kind == ActionKind::AckAlert)},
          }));
        }
      }
    }
  }
  JsonValue::Array pages;
  for (const char* page : kLocalPages) pages.push_back(JsonValue::makeString(page));
  return JsonValue::makeObject({
      {"schema", JsonValue::makeString("eego.input-bindings/1")},
      {"required", JsonValue::makeArray({JsonValue::makeString("schema_version"),
                                         JsonValue::makeString("revision"),
                                         JsonValue::makeString("bindings")})},
      {"max_bindings", JsonValue::makeInt(kMaxInputBindings)},
      {"triggers", JsonValue::makeArray(std::move(triggers))},
      {"local_pages", JsonValue::makeArray(std::move(pages))},
      {"dashboard_page_prefix", JsonValue::makeString("dashboard:")},
      {"builtins", JsonValue::makeArray(std::move(builtins))},
      {"registered_actions", JsonValue::makeArray(std::move(actions))},
      {"reserved", JsonValue::makeArray({JsonValue::makeString("power.*"),
                                         JsonValue::makeString("front.short"),
                                         JsonValue::makeString("confirm"),
                                         JsonValue::makeString("recovery")})},
      {"confirmation", JsonValue::makeString("fresh_hold_700ms_same_target")},
  });
}

}  // namespace eego::admin
