// SPDX-License-Identifier: GPL-3.0-or-later
#include "admin/time_policy.h"
#include "test_framework.h"

using eego::admin::shouldApplyBrowserTime;

TEST(time_policy, browser_time_rules) {
  CHECK(shouldApplyBrowserTime(false, false, -1, 0));
  CHECK(shouldApplyBrowserTime(false, true, 10, 0));
  CHECK(!shouldApplyBrowserTime(true, false, -1, 2));
  CHECK(shouldApplyBrowserTime(true, false, -1, 3));
  CHECK(shouldApplyBrowserTime(true, false, -1, -30));
  CHECK(!shouldApplyBrowserTime(true, true, 3600, 600));
  CHECK(shouldApplyBrowserTime(true, true, 90000, 600));
}
