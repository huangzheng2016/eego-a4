// SPDX-License-Identifier: GPL-3.0-or-later
#include "dashboard_core/snapshot.h"

#include <utility>

namespace eego::dashboard {

std::shared_ptr<const DashboardSnapshot> SnapshotPublisher::publish(Dashboard dashboard,
                                                                    uint64_t generated_at) {
  std::lock_guard<std::mutex> lock(publish_mutex_);
  const uint64_t generation = next_generation_ + 1;
  std::shared_ptr<const DashboardSnapshot> next(
      new DashboardSnapshot(std::move(dashboard), generated_at, generation));
#ifdef DASHBOARD_CORE_HAS_ATOMIC_SHARED_PTR
  current_.store(next, std::memory_order_release);
#else
  std::atomic_store_explicit(&current_, std::shared_ptr<const DashboardSnapshot>(next),
                             std::memory_order_release);
#endif
  next_generation_ = generation;
  return next;
}

std::shared_ptr<const DashboardSnapshot> SnapshotPublisher::acquire() const {
#ifdef DASHBOARD_CORE_HAS_ATOMIC_SHARED_PTR
  return current_.load(std::memory_order_acquire);
#else
  return std::atomic_load_explicit(&current_, std::memory_order_acquire);
#endif
}

uint64_t SnapshotPublisher::publishedGeneration() const {
  const auto current = acquire();
  return current == nullptr ? 0 : current->generation();
}

}  // namespace eego::dashboard
