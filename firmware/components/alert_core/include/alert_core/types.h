// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// AlertRecord / AlertEvent and the ingest validation contract (M11 §4).
//
// Canonical record shape (persistence and API use the same form):
//   { "schema": "eego.alert/1", "id": <ULID>, "fingerprint": <sha1 hex>,
//     "type": "host.disk_almost_full", "severity": "critical|warning|info",
//     "source": { "id": "...", "kind": "http_poll|http_push|...", "ref": "..." },
//     "title": "...", "message": "...", "fields": { ... scalars ... },
//     "state": "active|acked|cleared|expired", "muted": false,
//     "first_seen": "2026-10-21T08:00:00Z", "last_seen": "...",
//     "occurrences": 3, "ack_at": null, "ack_by": null,
//     "expires_at": "...", "cleared_at": null }
//
// All timestamps are unix seconds UTC internally; 0 means "null" for the
// optional ones (ack_at / expires_at / cleared_at).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "alert_core/json.h"

namespace alert_core {

// ---------------------------------------------------------------------------
// Validation budgets (M11 §4; violations are field-level 400s, never clamps)
// ---------------------------------------------------------------------------
constexpr size_t kMaxTitleLen = 120;
constexpr size_t kMaxMessageLen = 2000;
constexpr size_t kMaxFieldsCount = 16;
constexpr size_t kMaxFieldKeyLen = 32;
constexpr size_t kMaxFieldValueLen = 256;
constexpr size_t kMaxEventJsonBytes = 16 * 1024;
constexpr size_t kMaxBatchEvents = 32;
constexpr size_t kMaxDedupKeys = 8;
constexpr size_t kMaxTypeIdLen = 64;
constexpr size_t kMaxSourceIdLen = 64;
constexpr size_t kFingerprintHexLen = 40;
constexpr size_t kDefaultMaxRecords = 500;
constexpr uint32_t kTtlCritSeconds = 7 * 24 * 3600;   // 604800
constexpr uint32_t kTtlWarnSeconds = 3 * 24 * 3600;   // 259200
constexpr uint32_t kTtlInfoSeconds = 24 * 3600;       // 86400
constexpr uint32_t kMaxTtlSeconds = 366 * 24 * 3600;  // sanity cap
constexpr char kAlertSchema[] = "eego.alert/1";

// Pseudo dedup-key names that resolve to record-level values instead of
// field keys.
constexpr char kDedupKeySourceId[] = "source.id";
constexpr char kDedupKeyType[] = "type";
constexpr char kDedupKeyTitle[] = "title";

enum class Severity : uint8_t {
  Info = 0,
  Warning = 1,
  Critical = 2,
};

enum class AlertState : uint8_t {
  Active = 0,
  Acked = 1,
  Cleared = 2,
  Expired = 3,
};

// Well-known source kind strings (stored as plain strings so future adapters
// like ble_scan/serial inject need no core change).
constexpr char kSourceKindHttpPoll[] = "http_poll";
constexpr char kSourceKindHttpPush[] = "http_push";
constexpr char kSourceKindBleScan[] = "ble_scan";
constexpr char kSourceKindSerialInject[] = "serial_inject";

const char* severityName(Severity s);
// Accepts exactly "critical" | "warning" | "info".
bool severityFromString(const std::string& s, Severity& out);
int severityRank(Severity s);
uint32_t defaultTtlForSeverity(Severity s);

const char* alertStateName(AlertState st);
bool alertStateFromString(const std::string& s, AlertState& out);

struct AlertSource {
  std::string id;
  std::string kind;
  std::string ref;
};

struct AlertRecord {
  std::string id;           // ULID, assigned by the store on creation
  std::string fingerprint;  // sha1 hex, see store.h for the exact recipe
  std::string type;         // type_id as ingested (unknown ids are legal)
  Severity severity = Severity::Info;
  AlertSource source;
  std::string title;
  std::string message;
  JsonValue fields;  // Object of scalars (possibly empty)
  AlertState state = AlertState::Active;
  bool muted = false;
  uint64_t first_seen = 0;
  uint64_t last_seen = 0;
  uint32_t occurrences = 1;
  uint64_t ack_at = 0;  // 0 = never acked
  std::string ack_by;
  uint64_t expires_at = 0;  // 0 = no expiry
  uint64_t cleared_at = 0;  // 0 = not cleared
};

// Normalized ingest event. Only `type` is required on the wire; everything
// else falls back to type-registry defaults or device-computed values.
struct AlertEvent {
  std::string type;
  bool has_severity = false;
  Severity severity = Severity::Info;
  std::string title;
  std::string message;
  JsonValue fields;  // Object; empty default
  bool cleared = false;  // event carried state="cleared"
  bool has_ttl = false;
  uint32_t ttl_s = 0;
  std::vector<std::string> dedup_key;  // empty → registry default → builtin default
  std::string fingerprint;             // empty → computed by the store
  uint64_t ts = 0;                     // 0 → device stamps
};

struct ValidationError {
  std::string field;  // "title", "fields.used_pct", "alerts[3].type", "" for body-level
  std::string message;
};

// Parses and validates one ingest event from an already-parsed JSON object.
// Unknown members are ignored (M11); unknown type ids are legal. On failure
// returns false with a field-level error (field path relative to the event).
bool eventFromJson(const JsonValue& obj, AlertEvent& out, ValidationError& err);

// Batch entry point: accepts either a single event object or
// { "alerts": [ ... ] } with at most kMaxBatchEvents entries. Error field
// paths are prefixed with "alerts[i]." for batch elements.
bool eventsFromJson(const JsonValue& root, std::vector<AlertEvent>& out, ValidationError& err);

// Convenience: parse + validate straight from a request body. The 16KB
// per-event budget is enforced here (body may exceed it only for batches,
// which the HTTP layer caps at kMaxBatchEvents * kMaxEventJsonBytes).
bool eventsFromJsonText(const std::string& body, std::vector<AlertEvent>& out,
                        ValidationError& err);

// Canonical record serialization (persistence + API shape).
JsonValue recordToJson(const AlertRecord& r);
std::string recordToJsonText(const AlertRecord& r);

// Tolerant record parser for persistence reload: ignores unknown members,
// requires id/fingerprint/type/state to be present and well-formed.
bool recordFromJson(const JsonValue& obj, AlertRecord& out);

// fingerprint = sha1(type | dedup-key values). Keys are field keys from
// `fields`, or the pseudo keys source.id/type/title resolved against
// `source_id`/`type`/`title`. Exposed for testing and for adapters that want
// to pre-compute identities.
std::string computeFingerprint(const std::string& type, const std::vector<std::string>& dedup_key,
                               const JsonValue& fields, const std::string& source_id,
                               const std::string& title);

}  // namespace alert_core
