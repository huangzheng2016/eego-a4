// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// AlertStore: the single-writer alert store (M11 §4 semantics, M12 ownership
// model). One ingest task owns it and performs every mutation; UI tasks never
// touch it — they read immutable snapshots (snapshot.h).
//
// Semantics implemented here:
//   * dedup: fingerprint = sha1(type | dedup-key values). A repeat hit updates
//     last_seen, bumps occurrences, overwrites title/message/fields with the
//     latest non-empty values and recomputes expires_at. Acked stays acked
//     (sticky); an escalation (severity rank increases) reopens to Active when
//     reopen_on_escalate is set; Cleared/Expired reopen to Active on re-fire.
//   * ack: Active → Acked with ack_at/ack_by ("panel", "api:<name>", ...).
//   * clear: event state=cleared or explicit clear → Cleared (kept for the
//     retention window). http_push records are never auto-cleared by the core.
//   * expire: expires_at = last_seen + effective TTL (event ttl_s wins, then
//     type override, then type default, then severity band); due → Expired.
//   * mute: rules by type id / source id / fingerprint, optional TTL; muted
//     records are stored and updated as usual but flagged muted (excluded from
//     overview counts and default lists).
//   * retention: ring of max_records (default 500). Overflow evicts oldest
//     Expired, then oldest Cleared, then oldest Acked; Active records are
//     never purged — when only Active records remain the ingest is rejected
//     with store_full (HTTP 503 upstream, never a silent drop).
//
// Persistence hook: attach an AlertDb (persistence.h) and every mutation is
// appended as a full-record JSONL line.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "alert_core/type_registry.h"
#include "alert_core/types.h"
#include "alert_core/ulid.h"

namespace alert_core {

class AlertDb;

struct IngestOutcome {
  enum class Status {
    Created,   // new record (HTTP 201 upstream)
    Updated,   // dedup hit on an existing record (HTTP 202 upstream)
    Rejected,  // store_full — capacity reached with only Active records (503)
  };
  Status status = Status::Rejected;
  std::string id;            // record id for Created/Updated
  bool store_full = false;   // Rejected because at capacity with all-Active
  bool persist_failed = false;  // mutation applied in memory but the alerts.db
                                // append failed — owner should log/5xx and can
                                // watch AlertStore::dbIoFailures()
};

struct MuteRule {
  enum class MatchBy {
    Type = 0,
    Source = 1,
    Fingerprint = 2,
  };
  MatchBy match_by = MatchBy::Type;
  std::string value;
  uint64_t expires_at = 0;  // 0 = no expiry
};

struct StoreStats {
  size_t total = 0;
  size_t active = 0;    // Active, non-muted
  size_t acked = 0;     // Acked, non-muted
  size_t cleared = 0;   // Cleared, non-muted
  size_t expired = 0;   // Expired, non-muted
  size_t muted = 0;     // all muted records regardless of state
  size_t critical = 0;  // Active+Acked by severity, non-muted
  size_t warning = 0;
  size_t info = 0;
};

struct StoreConfig {
  size_t max_records = kDefaultMaxRecords;
  bool reopen_on_escalate = true;
};

// Entropy hook for ULID generation (caller wires the platform RNG).
using EntropyFn = std::function<void(uint8_t* out, size_t n)>;

class AlertStore {
 public:
  // `registry` must outlive the store; `entropy` may be empty, in which case
  // ids use a deterministic internal counter fallback (host tools/tests).
  AlertStore(TypeRegistry& registry, EntropyFn entropy = EntropyFn());

  const StoreConfig& config() const { return config_; }
  void setConfig(const StoreConfig& cfg) { config_ = cfg; }

  // Ingests one normalized event from the given source at unix time `now`.
  IngestOutcome ingest(const AlertEvent& ev, const AlertSource& source, uint64_t now);

  // Marks Active records Acked. Returns false for unknown id / wrong state.
  bool ack(const std::string& id, const std::string& ack_by, uint64_t now);
  size_t ackAll(const std::string& ack_by, uint64_t now);
  // Clears Active/Acked records (state=cleared). Returns false if not found.
  bool clear(const std::string& id, uint64_t now);
  // Moves due Active/Acked records to Expired. Returns the number expired.
  size_t expire(uint64_t now);

  // Mute rules (replace wholesale; re-evaluates all records).
  void setMuteRules(std::vector<MuteRule> rules, uint64_t now);
  const std::vector<MuteRule>& muteRules() const { return mute_rules_; }

  // Ring retention: evicts down to max_records (oldest Expired → Cleared →
  // Acked; Active never evicted). Returns evicted count.
  size_t purge();

  // Read side (still confined to the owning task — UI uses snapshots).
  const AlertRecord* find(const std::string& id) const;
  const AlertRecord* findByFingerprint(const std::string& fingerprint) const;
  std::vector<const AlertRecord*> list(const std::string& state, const std::string& severity,
                                       const std::string& type, const std::string& source,
                                       bool include_muted, uint64_t since,
                                       size_t limit) const;
  StoreStats stats() const;
  size_t size() const { return entries_.size(); }

  // Active (non-terminal) id+fingerprint pairs of one source — the http_poll
  // adapter feeds this into its clear_after_misses bookkeeping.
  std::vector<std::pair<std::string, std::string>> activeIdsBySource(
      const std::string& source_id) const;

  // Full record copy (e.g. feeding buildSnapshot for the UI publisher).
  std::vector<AlertRecord> allRecords() const;

  // Persistence wiring (see persistence.h). With a db attached, every mutation
  // appends a full-record JSONL line; loadFrom replaces the whole store and
  // enforces the retention cap. dbIoFailures() surfaces alerts.db write
  // trouble so the owner can log/react instead of silently diverging.
  void attachDb(AlertDb* db) { db_ = db; }
  size_t loadFrom(AlertDb& db);
  size_t dbIoFailures() const;
  // Rewrites the whole db from memory (atomic tmp + read-back verify + rename).
  bool compactDb(std::string& err);

  // Effective TTL for an event against a type: event ttl_s > override ttl >
  // type default > severity band.
  uint32_t effectiveTtl(const AlertEvent& ev, const AlertTypeDescriptor& desc) const;

 private:
  struct Entry {
    AlertRecord record;
    uint64_t seq = 0;  // store-local activity clock: purge "oldest" = smallest seq
  };

  Entry* findEntry(const std::string& id);
  Entry* findEntryByFingerprint(const std::string& fingerprint);
  // Evicts the best single victim (oldest Expired → Cleared → Acked; Active
  // never evicted). Returns false when only Active records remain.
  bool evictOne();
  std::string newId(uint64_t now_seconds);
  bool evaluateMuted(const AlertRecord& r, uint64_t now) const;
  bool touch(Entry& e, uint64_t now);  // bump seq, append db line; false on IO failure
  bool markDirty(const AlertRecord& r);

  TypeRegistry& registry_;
  EntropyFn entropy_;
  StoreConfig config_;
  std::vector<Entry> entries_;
  std::vector<MuteRule> mute_rules_;
  UlidMonotonic ulid_;
  uint64_t seq_counter_ = 0;
  uint64_t id_fallback_counter_ = 0;
  AlertDb* db_ = nullptr;
};

}  // namespace alert_core
