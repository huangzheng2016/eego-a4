#include "alert_core/snapshot.h"

#include <algorithm>
#include <atomic>

namespace alert_core {
namespace {

int displayRank(const AlertRecord& r) {
  // Terminal records sink below all live ones, then severity, then recency.
  if (r.state == AlertState::Cleared || r.state == AlertState::Expired) return -1;
  return severityRank(r.severity);
}

}  // namespace

AlertSnapshot buildSnapshot(std::vector<AlertRecord> records, uint64_t now, uint64_t seq) {
  AlertSnapshot snap;
  snap.generated_at = now;
  snap.seq = seq;
  std::sort(records.begin(), records.end(), [](const AlertRecord& a, const AlertRecord& b) {
    const int ra = displayRank(a);
    const int rb = displayRank(b);
    if (ra != rb) return ra > rb;
    if (a.last_seen != b.last_seen) return a.last_seen > b.last_seen;
    return a.id < b.id;  // stable tiebreak
  });
  for (const AlertRecord& r : records) {
    if (r.muted || (r.state != AlertState::Active && r.state != AlertState::Acked)) continue;
    ++snap.summary.active_total;
    switch (r.severity) {
      case Severity::Critical:
        ++snap.summary.critical;
        break;
      case Severity::Warning:
        ++snap.summary.warning;
        break;
      default:
        ++snap.summary.info;
        break;
    }
  }
  snap.summary.generated_at = now;
  snap.records = std::move(records);
  return snap;
}

void SnapshotPublisher::publish(AlertSnapshot snap) {
  snap.seq = seq_.fetch_add(1, std::memory_order_relaxed) + 1;
  auto next = std::make_shared<const AlertSnapshot>(std::move(snap));
#ifdef ALERT_CORE_HAS_ATOMIC_SHARED_PTR
  current_.store(std::move(next), std::memory_order_release);
#else
  std::atomic_store_explicit(&current_, std::shared_ptr<const AlertSnapshot>(next),
                             std::memory_order_release);
#endif
}

std::shared_ptr<const AlertSnapshot> SnapshotPublisher::acquire() const {
#ifdef ALERT_CORE_HAS_ATOMIC_SHARED_PTR
  return current_.load(std::memory_order_acquire);
#else
  return std::atomic_load_explicit(&current_, std::memory_order_acquire);
#endif
}

}  // namespace alert_core
