// SPDX-License-Identifier: GPL-3.0-or-later
#include "alert_core/store.h"

#include <algorithm>
#include <cstring>

#include "alert_core/persistence.h"

namespace alert_core {
namespace {

// Default dedup keys (M11): source.id + type + title.
const std::vector<std::string>& defaultDedupKeys() {
  static const std::vector<std::string> kKeys = {kDedupKeySourceId, kDedupKeyType, kDedupKeyTitle};
  return kKeys;
}

constexpr uint64_t kFutureSkewAllowanceS = 300;  // client ts at most 5 min ahead

bool isTerminal(AlertState st) { return st == AlertState::Cleared || st == AlertState::Expired; }

}  // namespace

AlertStore::AlertStore(TypeRegistry& registry, EntropyFn entropy)
    : registry_(registry), entropy_(std::move(entropy)) {}

uint32_t AlertStore::effectiveTtl(const AlertEvent& ev, const AlertTypeDescriptor& desc) const {
  if (ev.has_ttl) return ev.ttl_s;
  if (desc.ttl_default_s != 0) return desc.ttl_default_s;
  return defaultTtlForSeverity(ev.has_severity ? ev.severity : desc.severity_default);
}

std::string AlertStore::newId(uint64_t now_seconds) {
  uint8_t entropy[16];
  if (entropy_) {
    entropy_(entropy, sizeof(entropy));
  } else {
    // Deterministic fallback for host tools: counter as big-endian entropy.
    std::memset(entropy, 0, sizeof(entropy));
    uint64_t c = ++id_fallback_counter_;
    for (int i = 0; i < 8; ++i) entropy[15 - i] = static_cast<uint8_t>((c >> (i * 8)) & 0xFF);
  }
  return ulid_.next(now_seconds * 1000, entropy);
}

AlertStore::Entry* AlertStore::findEntry(const std::string& id) {
  for (Entry& e : entries_) {
    if (e.record.id == id) return &e;
  }
  return nullptr;
}

AlertStore::Entry* AlertStore::findEntryByFingerprint(const std::string& fingerprint) {
  for (Entry& e : entries_) {
    if (e.record.fingerprint == fingerprint) return &e;
  }
  return nullptr;
}

bool AlertStore::markDirty(const AlertRecord& r) {
  if (db_ == nullptr) return true;
  return db_->appendRecord(r);
}

bool AlertStore::touch(Entry& e, uint64_t /*now*/) {
  e.seq = ++seq_counter_;
  return markDirty(e.record);
}

bool AlertStore::evaluateMuted(const AlertRecord& r, uint64_t now) const {
  // Type-level mute from config overrides acts as a permanent mute rule.
  if (registry_.isTypeMuted(r.type)) return true;
  for (const MuteRule& rule : mute_rules_) {
    if (rule.expires_at != 0 && rule.expires_at <= now) continue;
    bool match = false;
    switch (rule.match_by) {
      case MuteRule::MatchBy::Type:
        match = r.type == rule.value;
        break;
      case MuteRule::MatchBy::Source:
        match = r.source.id == rule.value;
        break;
      case MuteRule::MatchBy::Fingerprint:
        match = r.fingerprint == rule.value;
        break;
    }
    if (match) return true;
  }
  return false;
}

IngestOutcome AlertStore::ingest(const AlertEvent& ev, const AlertSource& source, uint64_t now) {
  IngestOutcome outcome;
  const AlertTypeDescriptor desc = registry_.resolve(ev.type);
  const Severity severity = ev.has_severity ? ev.severity : desc.severity_default;

  const std::vector<std::string>& keys =
      !ev.dedup_key.empty() ? ev.dedup_key
                            : (!desc.dedup_key.empty() ? desc.dedup_key : defaultDedupKeys());
  const std::string fingerprint =
      !ev.fingerprint.empty()
          ? ev.fingerprint
          : computeFingerprint(ev.type, keys, ev.fields, source.id, ev.title);

  // Client ts sanity: honor it, clamp future skew, never move last_seen back.
  uint64_t seen = ev.ts != 0 ? ev.ts : now;
  if (seen > now + kFutureSkewAllowanceS) seen = now;

  if (Entry* e = findEntryByFingerprint(fingerprint)) {
    AlertRecord& r = e->record;
    if (seen < r.last_seen) seen = r.last_seen;
    r.last_seen = seen;
    ++r.occurrences;
    if (!ev.title.empty()) r.title = ev.title;
    if (!ev.message.empty()) r.message = ev.message;
    if (ev.fields.type() == JsonValue::Type::Object) r.fields = ev.fields;
    r.source.kind = source.kind;
    r.source.ref = source.ref;
    if (ev.has_severity) {
      const bool escalated = severityRank(ev.severity) > severityRank(r.severity);
      r.severity = ev.severity;
      if (escalated && config_.reopen_on_escalate && r.state == AlertState::Acked) {
        r.state = AlertState::Active;
        r.ack_at = 0;
        r.ack_by.clear();
      }
    }
    if (isTerminal(r.state)) {
      // Re-fire after clear/expiry: the condition is back on the wire.
      r.state = AlertState::Active;
      r.cleared_at = 0;
    }
    if (ev.cleared) {
      r.state = AlertState::Cleared;
      r.cleared_at = now;
    }
    r.expires_at = seen + effectiveTtl(ev, desc);
    r.muted = evaluateMuted(r, now);
    outcome.persist_failed = !touch(*e, now);
    outcome.status = IngestOutcome::Status::Updated;
    outcome.id = r.id;
    return outcome;
  }

  // New record: enforce ring capacity before inserting. Make room for exactly
  // one record; when only Active records remain there is nothing to evict.
  if (entries_.size() >= config_.max_records && !evictOne()) {
    outcome.status = IngestOutcome::Status::Rejected;
    outcome.store_full = true;
    return outcome;
  }

  Entry e;
  e.record.id = newId(now);
  e.record.fingerprint = fingerprint;
  e.record.type = ev.type;
  e.record.severity = severity;
  e.record.source = source;
  e.record.title = ev.title;
  e.record.message = ev.message;
  e.record.fields = ev.fields.type() == JsonValue::Type::Object ? ev.fields
                                                                : JsonValue::makeObject({});
  e.record.state = ev.cleared ? AlertState::Cleared : AlertState::Active;
  e.record.first_seen = seen;
  e.record.last_seen = seen;
  e.record.occurrences = 1;
  e.record.expires_at = seen + effectiveTtl(ev, desc);
  e.record.cleared_at = ev.cleared ? now : 0;
  e.record.muted = evaluateMuted(e.record, now);
  entries_.push_back(std::move(e));
  outcome.persist_failed = !touch(entries_.back(), now);
  outcome.status = IngestOutcome::Status::Created;
  outcome.id = entries_.back().record.id;
  return outcome;
}

bool AlertStore::ack(const std::string& id, const std::string& ack_by, uint64_t now) {
  Entry* e = findEntry(id);
  if (e == nullptr || e->record.state != AlertState::Active) return false;
  e->record.state = AlertState::Acked;
  e->record.ack_at = now;
  e->record.ack_by = ack_by;
  touch(*e, now);
  return true;
}

size_t AlertStore::ackAll(const std::string& ack_by, uint64_t now) {
  size_t count = 0;
  for (Entry& e : entries_) {
    if (e.record.state == AlertState::Active) {
      e.record.state = AlertState::Acked;
      e.record.ack_at = now;
      e.record.ack_by = ack_by;
      touch(e, now);
      ++count;
    }
  }
  return count;
}

bool AlertStore::clear(const std::string& id, uint64_t now) {
  Entry* e = findEntry(id);
  if (e == nullptr || isTerminal(e->record.state)) return false;
  e->record.state = AlertState::Cleared;
  e->record.cleared_at = now;
  touch(*e, now);
  return true;
}

size_t AlertStore::expire(uint64_t now) {
  size_t count = 0;
  for (Entry& e : entries_) {
    AlertRecord& r = e.record;
    if ((r.state == AlertState::Active || r.state == AlertState::Acked) && r.expires_at != 0 &&
        r.expires_at <= now) {
      r.state = AlertState::Expired;
      touch(e, now);
      ++count;
    }
  }
  return count;
}

void AlertStore::setMuteRules(std::vector<MuteRule> rules, uint64_t now) {
  mute_rules_ = std::move(rules);
  for (Entry& e : entries_) {
    const bool muted = evaluateMuted(e.record, now);
    if (muted != e.record.muted) {
      e.record.muted = muted;
      touch(e, now);
    }
  }
}

bool AlertStore::evictOne() {
  // Eviction order: oldest Expired → oldest Cleared → oldest Acked.
  // Active records are never evicted (M11); no victim means only they remain.
  Entry* victim = nullptr;
  for (int phase = 0; phase < 3 && victim == nullptr; ++phase) {
    for (Entry& e : entries_) {
      const AlertState st = e.record.state;
      const bool candidate =
          (phase == 0 && st == AlertState::Expired) ||
          (phase == 1 && st == AlertState::Cleared) || (phase == 2 && st == AlertState::Acked);
      if (candidate && (victim == nullptr || e.seq < victim->seq)) victim = &e;
    }
  }
  if (victim == nullptr) return false;
  // Persist the eviction: a tombstone line keeps the ring retention durable
  // across reboot (load honors tombstones, so this id never resurrects).
  const std::string evicted_id = victim->record.id;
  if (victim != &entries_.back()) {
    victim->record = entries_.back().record;
    victim->seq = entries_.back().seq;
  }
  entries_.pop_back();
  if (db_ != nullptr) db_->appendTombstone(evicted_id);
  return true;
}

size_t AlertStore::purge() {
  size_t evicted = 0;
  while (entries_.size() > config_.max_records && evictOne()) ++evicted;
  return evicted;
}

const AlertRecord* AlertStore::find(const std::string& id) const {
  for (const Entry& e : entries_) {
    if (e.record.id == id) return &e.record;
  }
  return nullptr;
}

std::vector<const AlertRecord*> AlertStore::list(const std::string& state,
                                                 const std::string& severity,
                                                 const std::string& type,
                                                 const std::string& source,
                                                 bool include_muted, uint64_t since,
                                                 size_t limit) const {
  std::vector<const AlertRecord*> out;
  for (const Entry& e : entries_) {
    const AlertRecord& r = e.record;
    if (!state.empty() && alertStateName(r.state) != state) continue;
    if (!severity.empty() && severityName(r.severity) != severity) continue;
    if (!type.empty() && r.type != type) continue;
    if (!source.empty() && r.source.id != source) continue;
    if (!include_muted && r.muted) continue;
    if (since != 0 && r.last_seen < since) continue;
    out.push_back(&r);
    if (limit != 0 && out.size() >= limit) break;
  }
  return out;
}

StoreStats AlertStore::stats() const {
  StoreStats s;
  s.total = entries_.size();
  for (const Entry& e : entries_) {
    const AlertRecord& r = e.record;
    if (r.muted) ++s.muted;  // muted counted once, regardless of state
    switch (r.state) {
      case AlertState::Cleared:
        if (!r.muted) ++s.cleared;
        break;
      case AlertState::Expired:
        if (!r.muted) ++s.expired;
        break;
      case AlertState::Acked:
        if (!r.muted) {
          ++s.acked;
          if (r.severity == Severity::Critical) ++s.critical;
          if (r.severity == Severity::Warning) ++s.warning;
          if (r.severity == Severity::Info) ++s.info;
        }
        break;
      case AlertState::Active:
        if (!r.muted) {
          ++s.active;
          if (r.severity == Severity::Critical) ++s.critical;
          if (r.severity == Severity::Warning) ++s.warning;
          if (r.severity == Severity::Info) ++s.info;
        }
        break;
    }
  }
  return s;
}

std::vector<AlertRecord> AlertStore::allRecords() const {
  std::vector<AlertRecord> out;
  out.reserve(entries_.size());
  for (const Entry& e : entries_) out.push_back(e.record);
  return out;
}

size_t AlertStore::loadFrom(AlertDb& db) {
  attachDb(&db);  // boot flow: keep appending to the same db after reload
  std::vector<AlertRecord> records;
  size_t malformed = 0;
  db.load(records, malformed);
  entries_.clear();
  entries_.reserve(records.size());
  for (AlertRecord& r : records) {
    Entry e;
    e.record = std::move(r);
    e.seq = ++seq_counter_;
    entries_.push_back(std::move(e));
  }
  // Boot-time retention: tombstones already prevent evicted ids from coming
  // back; this also enforces the (possibly re-configured) cap on stored dbs,
  // appending tombstones for anything it has to drop. Returns the live record
  // count after enforcement.
  purge();
  return entries_.size();
}

size_t AlertStore::dbIoFailures() const { return db_ != nullptr ? db_->ioFailures() : 0; }

bool AlertStore::compactDb(std::string& err) {
  if (db_ == nullptr) return false;
  // Visitor over the live entries — no copy of the record set (internal RAM
  // only on device). Single-owner store: nothing mutates entries_ meanwhile.
  const size_t count = entries_.size();
  if (!db_->compact(
          count,
          [this, count](size_t i) -> const AlertRecord* {
            return (i < count && i < entries_.size()) ? &entries_[i].record : nullptr;
          },
          err)) {
    return false;
  }
  size_t bytes = 0;
  last_compact_bytes_ = db_->fileSize(bytes) ? bytes : 0;
  return true;
}

bool AlertStore::maybeCompactDb(const CompactPolicy& policy, std::string& err, bool* attempted) {
  if (attempted != nullptr) *attempted = false;
  if (db_ == nullptr) return false;
  size_t bytes = 0;
  const bool have_size = db_->fileSize(bytes);
  const size_t dead = db_->deadLines();
  const size_t total = db_->totalLines();
  const bool too_big = have_size && bytes > policy.max_bytes && bytes > 2 * last_compact_bytes_;
  const bool mostly_dead = dead >= policy.min_dead_lines && dead * 2 > total;
  if (!too_big && !mostly_dead) return false;
  if (attempted != nullptr) *attempted = true;
  return compactDb(err);
}

}  // namespace alert_core
