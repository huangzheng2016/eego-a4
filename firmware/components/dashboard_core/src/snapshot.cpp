// SPDX-License-Identifier: GPL-3.0-or-later
#include "dashboard_core/snapshot.h"

#include <utility>

namespace eego::dashboard {

std::shared_ptr<const DashboardSnapshot> SnapshotPublisher::publish(Dashboard dashboard,
                                                                    uint64_t generated_at) {
  const uint64_t generation = generation_.fetch_add(1, std::memory_order_relaxed) + 1;
  std::shared_ptr<const DashboardSnapshot> next(
      new DashboardSnapshot(std::move(dashboard), generated_at, generation));
#ifdef DASHBOARD_CORE_HAS_ATOMIC_SHARED_PTR
  current_.store(next, std::memory_order_release);
#else
  std::atomic_store_explicit(&current_, std::shared_ptr<const DashboardSnapshot>(next),
                             std::memory_order_release);
#endif
  return next;
}

std::shared_ptr<const DashboardSnapshot> SnapshotPublisher::acquire() const {
#ifdef DASHBOARD_CORE_HAS_ATOMIC_SHARED_PTR
  return current_.load(std::memory_order_acquire);
#else
  return std::atomic_load_explicit(&current_, std::memory_order_acquire);
#endif
}

}  // namespace eego::dashboard
