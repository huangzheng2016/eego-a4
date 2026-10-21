// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include "dashboard_core/model.h"

#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
#define DASHBOARD_CORE_HAS_ATOMIC_SHARED_PTR 1
#endif

namespace eego::dashboard {

class DashboardSnapshot {
 public:
  const Dashboard& dashboard() const { return dashboard_; }
  uint64_t generatedAt() const { return generated_at_; }
  uint64_t generation() const { return generation_; }

 private:
  DashboardSnapshot(Dashboard dashboard, uint64_t generated_at, uint64_t generation)
      : dashboard_(std::move(dashboard)), generated_at_(generated_at), generation_(generation) {}

  Dashboard dashboard_;
  uint64_t generated_at_;
  uint64_t generation_;

  friend class SnapshotPublisher;
};

class SnapshotPublisher {
 public:
  // Deep-owns the moved model. Readers holding an older generation are not
  // disturbed, and acquire() never exposes mutable snapshot state.
  std::shared_ptr<const DashboardSnapshot> publish(Dashboard dashboard,
                                                   uint64_t generated_at);
  std::shared_ptr<const DashboardSnapshot> acquire() const;
  uint64_t publishedGeneration() const { return generation_.load(std::memory_order_relaxed); }

 private:
#ifdef DASHBOARD_CORE_HAS_ATOMIC_SHARED_PTR
  mutable std::atomic<std::shared_ptr<const DashboardSnapshot>> current_;
#else
  mutable std::shared_ptr<const DashboardSnapshot> current_;
#endif
  mutable std::atomic<uint64_t> generation_{0};
};

}  // namespace eego::dashboard
