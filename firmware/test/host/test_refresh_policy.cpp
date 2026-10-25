// SPDX-License-Identifier: GPL-3.0-or-later
// main/refresh_policy: idle-aware display cadence (wall-clock boundaries,
// idle transition, coalescing, critical exception, interaction wake).
#include <string>
#include <vector>

#include "refresh_policy.h"
#include "test_framework.h"

using panel::nextRefreshBoundary;
using panel::RefreshPolicy;
using panel::RefreshPolicyConfig;

namespace {

// 2026-10-25 02:00:00 UTC (= 10:00:00 at UTC+8).
constexpr uint64_t kBase = 1792893600ULL;
constexpr int64_t kMin = 60000;

RefreshPolicy makePolicy(uint32_t after = 10, uint32_t every = 5, bool critical = true,
                         int tz = 480) {
  RefreshPolicyConfig c;
  c.idle_after_min = after;
  c.idle_refresh_min = every;
  c.critical_immediate = critical;
  c.timezone_offset_min = tz;
  RefreshPolicy p;
  p.configure(c);
  p.noteInteraction(0);
  return p;
}

// Wall clock tracking the monotonic clock: unix = kBase + offset_s + mono/1000.
uint64_t wall(int64_t mono_ms, int64_t offset_s = 0) {
  return static_cast<uint64_t>(static_cast<int64_t>(kBase) + offset_s + mono_ms / 1000);
}

}  // namespace

TEST(refresh_policy, boundary_alignment_utc) {
  CHECK_EQ(kBase % 3600, 0u);
  CHECK_EQ(nextRefreshBoundary(kBase, 0, 5), kBase + 300);       // exactly on a boundary → next
  CHECK_EQ(nextRefreshBoundary(kBase + 1, 0, 5), kBase + 300);
  CHECK_EQ(nextRefreshBoundary(kBase + 299, 0, 5), kBase + 300);
  CHECK_EQ(nextRefreshBoundary(kBase + 300, 0, 5), kBase + 600);
  CHECK_EQ(nextRefreshBoundary(kBase + 61, 0, 2), kBase + 120);
  CHECK_EQ(nextRefreshBoundary(kBase + 10, 0, 60), kBase + 3600);
  CHECK_EQ(nextRefreshBoundary(kBase + 10, 0, 0), kBase + 60);   // 0 treated as 1 minute
}

TEST(refresh_policy, boundary_alignment_respects_timezone) {
  // Whole-hour offsets keep 5-minute boundaries on the same instants.
  CHECK_EQ(nextRefreshBoundary(kBase + 7, 480, 5), kBase + 300);
  // UTC+5:45 (Nepal): local = UTC + 20700 s; local 07:45 at kBase → 07:50 local.
  CHECK_EQ(nextRefreshBoundary(kBase + 7, 345, 5), kBase + 300);
  // Hourly boundary at UTC+5:45 lands at :15 UTC.
  CHECK_EQ(nextRefreshBoundary(kBase + 7, 345, 60), kBase + 900);
  // 30-minute boundary at UTC+5:30 (India): local xx:30 boundary = kBase in UTC terms.
  CHECK_EQ(nextRefreshBoundary(kBase, 330, 30), kBase + 1800);
  CHECK_EQ(nextRefreshBoundary(kBase - 1, 330, 30), kBase);
  // Negative offsets (UTC-3:30, Newfoundland), hourly boundary at :30 UTC.
  CHECK_EQ(nextRefreshBoundary(kBase + 100, -210, 60), kBase + 1800);
  // 15-minute boundaries with a 45-minute offset.
  CHECK_EQ(nextRefreshBoundary(kBase + 1, 45, 15), kBase + 900);
}

TEST(refresh_policy, active_then_idle_transition) {
  RefreshPolicy p = makePolicy(10, 5);
  CHECK(!p.idle(0));
  CHECK(!p.idle(10 * kMin - 1));
  CHECK(p.idle(10 * kMin));
  // Active: everything displays immediately.
  CHECK(p.allow(3 * kMin, wall(3 * kMin), false));
  CHECK(!p.deferred());
}

TEST(refresh_policy, disabled_is_always_active) {
  RefreshPolicy p = makePolicy(0, 5);
  CHECK(!p.idle(1000 * kMin));
  CHECK(p.allow(1000 * kMin + 1234, wall(1000 * kMin + 1234), false));
}

TEST(refresh_policy, idle_coalesces_until_boundary) {
  RefreshPolicy p = makePolicy(10, 5);
  // Last render at 10:09:40 local (Active), idle from 10:10:00.
  p.noteRendered(wall(9 * kMin + 40000));
  // Data arrives at 10:11:07 local → deferred to 10:15:00.
  const int64_t t1 = 11 * kMin + 7000;
  CHECK(!p.allow(t1, wall(t1), false));
  CHECK(p.deferred());
  CHECK_EQ(p.dueUnix(), kBase + 900);
  // More updates (clock minutes, other panels) keep the same single deadline.
  for (int64_t t = t1; t < 15 * kMin; t += 997) CHECK(!p.allow(t, wall(t), false));
  CHECK_EQ(p.dueUnix(), kBase + 900);
  // Exactly at the boundary → one redraw.
  CHECK(p.allow(15 * kMin, wall(15 * kMin), false));
  p.noteRendered(wall(15 * kMin));
  CHECK(!p.deferred());
  // Next update is coalesced into 10:20.
  const int64_t t2 = 15 * kMin + 2000;
  CHECK(!p.allow(t2, wall(t2), false));
  CHECK_EQ(p.dueUnix(), kBase + 1200);
  CHECK(!p.allow(20 * kMin - 1, wall(20 * kMin - 1), false));
  CHECK(p.allow(20 * kMin, wall(20 * kMin), false));
}

TEST(refresh_policy, boundary_minute_change_renders_in_grace) {
  // Clock minute flips exactly at 10:15:00 with nothing pending before it: the
  // boundary redraw must still happen (clock correct at boundaries).
  RefreshPolicy p = makePolicy(10, 5);
  p.noteRendered(wall(14 * kMin + 1000));  // 10:14:01 (Active render; idle from 10:10 anyway)
  const int64_t t = 15 * kMin + 30;        // 10:15:00.03
  CHECK(p.allow(t, wall(t), false));
  p.noteRendered(wall(t));
  // A later update within the same boundary minute waits for the next boundary.
  const int64_t t2 = 15 * kMin + 20000;
  CHECK(!p.allow(t2, wall(t2), false));
  CHECK_EQ(p.dueUnix(), kBase + 1200);
}

TEST(refresh_policy, idle_entry_does_not_render_off_boundary) {
  // Active render at 10:03; idle from 10:13 (idle_after=10). A data update at
  // 10:13 must wait for 10:15, not render immediately.
  RefreshPolicy p = makePolicy(10, 5);
  p.noteInteraction(3 * kMin);
  p.noteRendered(wall(3 * kMin));
  const int64_t t = 13 * kMin + 5000;
  CHECK(p.idle(t));
  CHECK(!p.allow(t, wall(t), false));
  CHECK_EQ(p.dueUnix(), kBase + 900);
}

TEST(refresh_policy, timezone_offset_shifts_boundary) {
  // UTC+5:45 local boundaries are :50 local = kBase+300 in this setup; hourly
  // boundaries fall at :15 UTC.
  RefreshPolicy p = makePolicy(1, 60, true, 345);
  p.noteRendered(wall(0));
  const int64_t t = 2 * kMin;
  CHECK(!p.allow(t, wall(t), false));
  CHECK_EQ(p.dueUnix(), kBase + 900);
  CHECK(!p.allow(15 * kMin - 1000, wall(15 * kMin - 1000), false));
  CHECK(p.allow(15 * kMin, wall(15 * kMin), false));
}

TEST(refresh_policy, critical_alert_exception) {
  RefreshPolicy p = makePolicy(10, 5, true);
  p.noteRendered(wall(9 * kMin), {"src\nold-critical"});
  const int64_t t = 12 * kMin;
  // Already-displayed critical alerts do not bypass the cadence.
  CHECK(!p.criticalArrived({"src\nold-critical"}));
  CHECK(!p.allow(t, wall(t), p.criticalArrived({"src\nold-critical"})));
  // A newly active critical alert redraws immediately.
  const std::vector<std::string> now_active = {"src\nold-critical", "src\nnew-critical"};
  CHECK(p.criticalArrived(now_active));
  CHECK(p.allow(t + 1000, wall(t + 1000), p.criticalArrived(now_active)));
  p.noteRendered(wall(t + 1000), now_active);
  CHECK(!p.criticalArrived(now_active));
  // Resolved then re-raised counts as new again.
  p.noteRendered(wall(t + 2000), {});
  CHECK(p.criticalArrived({"src\nnew-critical"}));
}

TEST(refresh_policy, critical_exception_can_be_disabled) {
  RefreshPolicy p = makePolicy(10, 5, false);
  p.noteRendered(wall(9 * kMin), {});
  CHECK(!p.criticalArrived({"src\nnew-critical"}));
  const int64_t t = 12 * kMin;
  CHECK(!p.allow(t, wall(t), true));  // caller's flag ignored when disabled
}

TEST(refresh_policy, interaction_wakes_immediately) {
  RefreshPolicy p = makePolicy(10, 5);
  p.noteRendered(wall(9 * kMin));
  const int64_t t = 12 * kMin;
  CHECK(!p.allow(t, wall(t), false));
  CHECK(p.deferred());
  p.noteInteraction(t + 500);
  CHECK(!p.idle(t + 500));
  CHECK(!p.deferred());
  CHECK(p.allow(t + 600, wall(t + 600), false));
  // Idle again after another idle_after_min without input.
  CHECK(!p.idle(t + 500 + 10 * kMin - 1));
  CHECK(p.idle(t + 500 + 10 * kMin));
}

TEST(refresh_policy, invalid_time_falls_back_to_monotonic_interval) {
  RefreshPolicy p = makePolicy(10, 5);
  const int64_t t = 11 * kMin;
  CHECK(!p.allow(t, 0, false));
  CHECK_EQ(p.dueUnix(), 0u);
  CHECK(!p.allow(t + 5 * kMin - 1, 0, false));
  CHECK(p.allow(t + 5 * kMin, 0, false));
}

TEST(refresh_policy, wall_clock_jump_back_is_capped) {
  RefreshPolicy p = makePolicy(10, 5);
  p.noteRendered(wall(9 * kMin));
  const int64_t t = 11 * kMin;
  CHECK(!p.allow(t, wall(t), false));  // due 10:15
  // SNTP steps the clock back one hour: wall boundary is now far away, but the
  // monotonic cap (distance + one interval) still forces a redraw.
  const int64_t later = t + 4 * kMin + 5 * kMin;
  CHECK(!p.allow(later - 1000, wall(later - 1000, -3600), false));
  CHECK(p.allow(later, wall(later, -3600), false));
}

TEST(refresh_policy, reconfigure_reschedules) {
  RefreshPolicy p = makePolicy(10, 5);
  p.noteRendered(wall(9 * kMin));
  const int64_t t = 11 * kMin;
  CHECK(!p.allow(t, wall(t), false));
  CHECK_EQ(p.dueUnix(), kBase + 900);
  RefreshPolicyConfig c = p.config();
  c.idle_refresh_min = 15;
  p.configure(c);
  CHECK(!p.deferred());
  CHECK(!p.allow(t + 1000, wall(t + 1000), false));
  CHECK_EQ(p.dueUnix(), kBase + 900);  // 10:15 is also a 15-minute boundary
  c.idle_refresh_min = 30;
  p.configure(c);
  CHECK(!p.allow(t + 2000, wall(t + 2000), false));
  CHECK_EQ(p.dueUnix(), kBase + 1800);
  // Disabling idle makes everything immediate again.
  c.idle_after_min = 0;
  p.configure(c);
  CHECK(p.allow(t + 3000, wall(t + 3000), false));
}
