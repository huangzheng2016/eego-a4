// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Alert type registry: 39 built-in preset types + generic.any fallback (M11 §3).
//
// Each descriptor carries everything the UI and the store need, bilingual:
//   id, name_zh, name_en, severity_default, icon_key (UI-resolved 1bpp
//   bitmap), table_cols (field keys shown in table rows), detail_fields
//   (field keys shown in the detail view), dedup_key, ttl_default_s, group.
//
// Remote config can override per id (severity / ttl / aliases / mute) via
// config.json "alert_type_overrides{}" — see applyOverridesJson. Unknown ids
// are legal everywhere: resolve() falls back to the generic.any descriptor
// while keeping the queried id so dedup and TTL stay per-id.

#include <cstdint>
#include <string>
#include <vector>

#include "alert_core/json.h"
#include "alert_core/types.h"

namespace alert_core {

struct AlertTypeDescriptor {
  std::string id;
  std::string name_zh;
  std::string name_en;
  Severity severity_default = Severity::Info;
  std::string icon_key;                    // key into the UI's 1bpp icon set
  std::vector<std::string> table_cols;     // field keys for table rows
  std::vector<std::string> detail_fields;  // field keys for the detail view
  std::vector<std::string> dedup_key;      // empty → builtin default keys
  uint32_t ttl_default_s = 0;              // 0 → derive from severity band
  // Group: system|network|service|power_env|security|home|self|generic.
  std::string group;
  // Type-level mute from config overrides. Consumed inside alert_core:
  // AlertStore treats it as a permanent mute rule for the type (records are
  // stored/updated as usual but flagged muted). Surfaced here so API/UI
  // consumers of resolve() see the effective value.
  bool muted_default = false;
};

// Config overrides for one type id (config.json "alert_type_overrides{}").
// All members optional; severity/ttl/names apply via TypeRegistry::resolve();
// muted is wired into AlertStore's mute evaluation (see descriptor above).
struct AlertTypeOverride {
  bool has_severity = false;
  Severity severity = Severity::Info;
  bool has_ttl = false;
  uint32_t ttl_s = 0;
  bool has_name_zh = false;
  std::string name_zh;
  bool has_name_en = false;
  std::string name_en;
  bool has_muted = false;
  bool muted = false;

  bool empty() const {
    return !has_severity && !has_ttl && !has_name_zh && !has_name_en && !has_muted;
  }
};

class TypeRegistry {
 public:
  TypeRegistry();

  // The 40 built-ins (39 presets + generic.any), in stable order.
  const std::vector<AlertTypeDescriptor>& builtins() const { return builtins_; }
  const AlertTypeOverride* findOverride(const std::string& type_id) const;
  bool isKnown(const std::string& type_id) const;
  const AlertTypeDescriptor* findBuiltin(const std::string& type_id) const;
  // True when the override for `type_id` sets muted:true — AlertStore applies
  // it as a type-level mute (see AlertTypeDescriptor::muted_default).
  bool isTypeMuted(const std::string& type_id) const;

  // Effective descriptor for an id: built-in merged with override when known;
  // otherwise generic.any with the queried id substituted (rendering falls
  // back to the generic template, dedup/TTL stay per-id). ttl_default_s of 0
  // means "use the severity band" (generic.any and overrides may rely on it).
  AlertTypeDescriptor resolve(const std::string& type_id) const;

  // Overrides (persisted by the caller into config.json).
  void setOverride(const std::string& type_id, const AlertTypeOverride& o);
  bool removeOverride(const std::string& type_id);
  void clearOverrides();

  // Parses the config.json "alert_type_overrides{}" shape:
  //   { "<type_id>": { "severity": "...", "ttl_s": 604800, "name_zh": "...",
  //                    "name_en": "...", "muted": true } }
  // All members optional; unknown members ignored; invalid values error.
  bool applyOverridesJson(const JsonValue& obj, ValidationError& err);

  static const AlertTypeDescriptor& generic();

 private:
  std::vector<AlertTypeDescriptor> builtins_;
  std::vector<std::pair<std::string, AlertTypeOverride>> overrides_;
};

}  // namespace alert_core
