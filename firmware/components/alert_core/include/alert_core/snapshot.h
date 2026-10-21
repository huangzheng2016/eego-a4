#pragma once

// Immutable alert snapshots + double-buffered publishing (M12: "alert_core
// publishes immutable snapshots via double-buffer; app_task renders snapshots
// only").
//
// The ingest task rebuilds a snapshot after mutations and publishes it; UI
// tasks acquire the current one without any lock:
//
//   auto snap = publisher.acquire();   // wait-free shared_ptr atomic load
//   for (const AlertRecord& r : snap->records) ...
//
// The previous generation stays alive for readers still holding it — that is
// the second buffer. Snapshots are never mutated after publication.

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "alert_core/types.h"

// C++20 offers std::atomic<std::shared_ptr<T>>; older hosts use the C++11
// free atomic_store/atomic_load functions on shared_ptr (deprecated but
// fully functional in C++20 toolchains — hence the feature test).
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
#define ALERT_CORE_HAS_ATOMIC_SHARED_PTR 1
#endif

namespace alert_core {

// Overview counts (S0 badge row). Muted records are excluded by contract (M11:
// muted alerts stay in the store but out of overview/badges/default lists).
struct AlertSummary {
  size_t critical = 0;
  size_t warning = 0;
  size_t info = 0;
  size_t active_total = 0;  // Active + Acked, non-muted
  uint64_t generated_at = 0;
};

struct AlertSnapshot {
  uint64_t generated_at = 0;  // unix seconds when the snapshot was built
  uint64_t seq = 0;           // publish counter, strictly increasing
  // All records in display order: non-terminal before terminal, then severity
  // rank (critical → info), then last_seen descending. Consumers filter via
  // state/muted for table views (muted and terminal rows sink accordingly).
  std::vector<AlertRecord> records;
  AlertSummary summary;
};

class SnapshotPublisher {
 public:
  // Writer side (ingest task). Takes ownership; the snapshot is immutable from
  // here on. Assigns a strictly increasing seq.
  void publish(AlertSnapshot snap);

  // Reader side (any task, lock-free). The returned pointer keeps the
  // generation alive; the next publish does not disturb it.
  std::shared_ptr<const AlertSnapshot> acquire() const;

  uint64_t publishedSeq() const { return seq_.load(std::memory_order_relaxed); }

 private:
#ifdef ALERT_CORE_HAS_ATOMIC_SHARED_PTR
  mutable std::atomic<std::shared_ptr<const AlertSnapshot>> current_;
#else
  mutable std::shared_ptr<const AlertSnapshot> current_;
#endif
  mutable std::atomic<uint64_t> seq_{0};
};

// Builds a snapshot from the store's current records (called by the ingest
// task; store.h stays the single-writer owner of the data).
AlertSnapshot buildSnapshot(std::vector<AlertRecord> records, uint64_t now, uint64_t seq);

}  // namespace alert_core
