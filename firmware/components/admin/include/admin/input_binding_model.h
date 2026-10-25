// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "alert_core/json.h"
#include "dashboard_core/model.h"

namespace eego::admin {

inline constexpr int64_t kInputBindingSchemaVersion = 1;
inline constexpr size_t kMaxInputBindings = 24;

enum class InputTrigger : uint8_t {
  UpShort,
  UpLong,
  UpDouble,
  DownShort,
  DownLong,
  DownDouble,
  UpDownChord,
  FrontLong,
  FrontDouble,
};

const char* inputTriggerName(InputTrigger trigger);
bool inputTriggerFromString(std::string_view text, InputTrigger& out);

enum class InputActionKind : uint8_t { None, Builtin, DashboardAction };

struct InputAction {
  InputActionKind kind = InputActionKind::None;
  std::string target;
  std::string panel_id;
  std::string action_id;
};

struct InputBinding {
  std::string page = "*";
  InputTrigger trigger = InputTrigger::UpShort;
  InputAction action;
};

struct InputBindingSet {
  uint32_t revision = 1;
  std::vector<InputBinding> bindings;
  bool loaded = false;
  bool recovered = false;
};

struct InputBindingError {
  std::string field;
  std::string message;
};
using InputBindingErrors = std::vector<InputBindingError>;

bool operator==(const InputAction& left, const InputAction& right);
bool operator==(const InputBinding& left, const InputBinding& right);

bool inputBuiltinAllowed(std::string_view name);
const std::vector<InputBinding>& defaultInputBindings();

bool inputBindingsFromJson(const alert_core::JsonValue& root, bool require_revision,
                           InputBindingSet& out, InputBindingErrors& errors);
// Parses the persisted NVS record (JSON text). False when it is unparsable or
// no longer valid for this firmware (e.g. a page or builtin name was removed);
// `reason` then names the first problem. The caller falls back to defaults.
bool parseStoredInputBindings(const std::string& text, InputBindingSet& out, std::string& reason);
alert_core::JsonValue inputBindingToJson(const InputBinding& binding);
alert_core::JsonValue inputBindingListToJson(const std::vector<InputBinding>& bindings);
alert_core::JsonValue inputBindingsToJson(const InputBindingSet& set);

bool validateInputBindingsSyntax(const std::vector<InputBinding>& bindings,
                                 InputBindingErrors& errors);
bool validateInputBindings(const std::vector<InputBinding>& bindings,
                           const dashboard::Dashboard* dashboard,
                           InputBindingErrors& errors);

// Exact page, dashboard group, global override, then the built-in default profile.
// An explicit `none` action is returned and therefore disables lower-priority defaults.
bool resolveInputBinding(const std::vector<InputBinding>& overrides,
                         const std::string& page_context, InputTrigger trigger,
                         InputBinding& out);

alert_core::JsonValue inputBindingSchema(const dashboard::Dashboard* dashboard);

}  // namespace eego::admin
