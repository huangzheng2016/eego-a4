// SPDX-License-Identifier: GPL-3.0-or-later
// net_stack wifi_list: build-time defaults, seeding, admin list edits, attempt plan.
#include <string>
#include <vector>

#include "net_stack/wifi_list.h"
#include "test_framework.h"

using namespace eego::net;

namespace {

// Same shape local_secrets.h produces via EEGO_LOCAL_WIFI_LIST.
#define TEST_WIFI_LIST {{"home", "home-password"}, {"office", "office-password"}, {"cafe", ""}}
constexpr WifiDefault kList[] = TEST_WIFI_LIST;
static_assert(validWifiDefaults(kList), "list macro shape must validate");

constexpr WifiDefault kSingle[] = {{"only", "12345678"}};
static_assert(validWifiDefaults(kSingle), "single shorthand validates");

constexpr WifiDefault kShortPass[] = {{"home", "1234567"}};
static_assert(!validWifiDefaults(kShortPass), "password < 8 rejected");
constexpr WifiDefault kLongSsid[] = {{"123456789012345678901234567890123", ""}};
static_assert(!validWifiDefaults(kLongSsid), "ssid > 32 rejected");
constexpr WifiDefault kEmptySsid[] = {{"", "12345678"}};
static_assert(!validWifiDefaults(kEmptySsid), "empty ssid rejected");
constexpr WifiDefault kDup[] = {{"a", ""}, {"a", "12345678"}};
static_assert(!validWifiDefaults(kDup), "duplicate ssid rejected");
constexpr WifiDefault kTooMany[] = {{"1", ""}, {"2", ""}, {"3", ""}, {"4", ""}, {"5", ""},
                                    {"6", ""}, {"7", ""}, {"8", ""}, {"9", ""}};
static_assert(!validWifiDefaults(kTooMany), "more than kMaxWifiNetworks rejected");
static_assert(wifiCstrLen("adminpass") == 9, "admin password length check");
static_assert(kMaxWifiNetworks >= 5, "at least 5 networks");

std::vector<std::string> ssids(const std::vector<WifiAccount>& accounts) {
  std::vector<std::string> out;
  for (const auto& a : accounts) out.push_back(a.ssid);
  return out;
}

WifiListEntry keep(const std::string& ssid, const std::string& previous = "") {
  WifiListEntry e;
  e.ssid = ssid;
  e.keep_password = true;
  e.previous_ssid = previous;
  return e;
}

WifiListEntry set(const std::string& ssid, const std::string& password) {
  WifiListEntry e;
  e.ssid = ssid;
  e.password = password;
  return e;
}

}  // namespace

TEST(wifi_list, seeds_in_order_when_nvs_empty) {
  std::vector<WifiAccount> stored;
  CHECK(seedWifiDefaults(stored, kList, 3, false));
  CHECK(ssids(stored) == (std::vector<std::string>{"home", "office", "cafe"}));
  CHECK(stored[0].password == "home-password");
  CHECK(stored[2].password.empty());
}

TEST(wifi_list, defaults_always_ensured_in_front) {
  // Stale networks saved earlier (portal/older firmware) must not shadow defaults.
  std::vector<WifiAccount> stored = {{"mine", "my-password"}, {"office", "stale-password"}};
  CHECK(seedWifiDefaults(stored, kList, 3, false));
  CHECK(ssids(stored) == (std::vector<std::string>{"home", "office", "cafe", "mine"}));
  CHECK(stored[1].password == "office-password");  // build-time password wins
  CHECK(stored[3].password == "my-password");
  // Second boot: already ensured -> nothing to write.
  CHECK(!seedWifiDefaults(stored, kList, 3, false));
  // Reordered by the admin UI: defaults move back to the front, others keep order.
  stored = {{"x", ""}, {"cafe", ""}, {"y", ""}, {"home", "home-password"}};
  CHECK(seedWifiDefaults(stored, kList, 3, false));
  CHECK(ssids(stored) == (std::vector<std::string>{"home", "office", "cafe", "x", "y"}));
}

TEST(wifi_list, defaults_drop_lowest_priority_when_full) {
  std::vector<WifiAccount> stored;
  for (int i = 0; i < 8; i++) stored.push_back({"n" + std::to_string(i), ""});
  CHECK(seedWifiDefaults(stored, kList, 3, false));
  CHECK(stored.size() == kMaxWifiNetworks);
  CHECK(ssids(stored) ==
        (std::vector<std::string>{"home", "office", "cafe", "n0", "n1", "n2", "n3", "n4"}));
}

TEST(wifi_list, replace_keeps_only_defaults) {
  std::vector<WifiAccount> stored = {{"mine", "my-password"}};
  CHECK(seedWifiDefaults(stored, kList, 3, true));
  CHECK(ssids(stored) == (std::vector<std::string>{"home", "office", "cafe"}));
  CHECK(!seedWifiDefaults(stored, kList, 3, true));
}

TEST(wifi_list, seeding_skips_invalid_and_duplicates) {
  const WifiDefault mixed[] = {{"a", "short"}, {"b", "12345678"}, {"b", "87654321"}, {"", ""}};
  std::vector<WifiAccount> stored;
  CHECK(seedWifiDefaults(stored, mixed, 4, false));
  CHECK(ssids(stored) == std::vector<std::string>{"b"});
  CHECK(stored[0].password == "12345678");
  const WifiDefault bad[] = {{"a", "short"}};
  std::vector<WifiAccount> untouched = {{"keep", ""}};
  CHECK(!seedWifiDefaults(untouched, bad, 1, true));
  CHECK(ssids(untouched) == std::vector<std::string>{"keep"});
}

TEST(wifi_list, reason_codes_map_to_names_and_text) {
  CHECK(std::string(wifiReasonName(2)) == "AUTH_EXPIRE");
  CHECK(std::string(wifiReasonName(4)) == "ASSOC_EXPIRE");
  CHECK(std::string(wifiReasonName(8)) == "ASSOC_LEAVE");
  CHECK(std::string(wifiReasonName(15)) == "4WAY_HANDSHAKE_TIMEOUT");
  CHECK(std::string(wifiReasonName(201)) == "NO_AP_FOUND");
  CHECK(std::string(wifiReasonName(202)) == "AUTH_FAIL");
  CHECK(std::string(wifiReasonName(203)) == "ASSOC_FAIL");
  CHECK(std::string(wifiReasonName(204)) == "HANDSHAKE_TIMEOUT");
  CHECK(std::string(wifiReasonName(205)) == "CONNECTION_FAIL");
  CHECK(std::string(wifiReasonName(210)) == "NO_AP_FOUND_W_COMPATIBLE_SECURITY");
  CHECK(std::string(wifiReasonName(211)) == "NO_AP_FOUND_IN_AUTHMODE_THRESHOLD");
  CHECK(std::string(wifiReasonName(212)) == "NO_AP_FOUND_IN_RSSI_THRESHOLD");
  CHECK(std::string(wifiReasonName(99)) == "UNKNOWN");
  CHECK(std::string(wifiReasonText(15)).find("密码") != std::string::npos);
  CHECK(std::string(wifiReasonText(201)).find("5GHz") != std::string::npos);
  CHECK(std::string(wifiReasonText(0)).find("超时") != std::string::npos);
  CHECK(std::string(wifiReasonText(99)) == wifiReasonText(98));  // generic fallback
  CHECK(std::string(wifiReasonShort(15, false)) == "密码错误？");
  CHECK(std::string(wifiReasonShort(201, true)) == "NOT FOUND");
  CHECK(std::string(wifiReasonShort(123, true)) == "FAILED");
  CHECK(std::string(wifiAuthModeName(3)) == "WPA2_PSK");
  CHECK(std::string(wifiAuthModeName(0)) == "OPEN");
  CHECK(std::string(wifiAuthModeName(77)) == "OTHER");
}

TEST(wifi_list, diag_log_tracks_attempts_and_scan) {
  WifiDiagLog log;
  log.syncSaved({"home", "office"});
  log.recordScan(true,
                 {{"office", -80, 6, 3}, {"other", -40, 1, 0}, {"office", -60, 11, 3}, {"", -30, 1, 0}},
                 1000);
  const auto& snap = log.snapshot();
  CHECK(snap.scan.size() == 2);  // merged, hidden dropped
  CHECK(snap.scan[0].ssid == "other" && snap.scan[1].rssi == -60);
  CHECK(!snap.networks[0].seen);
  CHECK(snap.networks[1].seen && snap.networks[1].channel == 11);
  log.recordAttempt("home", 2000);
  CHECK(snap.networks[0].result == WifiAttemptResult::Connecting);
  log.recordResult("home", WifiAttemptResult::Failed, 201, 2500);
  CHECK(snap.networks[0].attempts == 1 && snap.networks[0].failures == 1);
  CHECK(snap.networks[0].last_reason == 201);
  CHECK(snap.networks[0].first_reason == 201 && snap.networks[0].first_failure_ms == 2500);
  log.recordResult("home", WifiAttemptResult::Failed, 2, 2550);
  CHECK(snap.networks[0].first_reason == 201 && snap.networks[0].last_reason == 2);
  log.recordResult("home", WifiAttemptResult::Failed, 201, 2560);
  log.recordResult("unknown", WifiAttemptResult::Failed, 2, 2600);  // ignored
  // Re-sync keeps history of surviving networks, adds new ones fresh.
  log.syncSaved({"cafe", "home"});
  CHECK(snap.networks.size() == 2 && snap.networks[1].last_reason == 201);
  CHECK(snap.networks[1].failures == 3);
  CHECK(snap.networks[0].attempts == 0);
  log.recordAttempt("home", 3000);
  log.recordResult("home", WifiAttemptResult::Connected, 0, 3100);
  CHECK(snap.networks[1].last_reason == 201);  // last failure reason kept for display
  CHECK(std::string(wifiAttemptResultName(snap.networks[1].result)) == "connected");
}

TEST(wifi_list, edit_reorders_keeps_and_renames) {
  const std::vector<WifiAccount> current = {{"a", "pass-aaaa"}, {"b", "pass-bbbb"}, {"c", ""}};
  std::vector<WifiAccount> out;
  WifiListError err;
  CHECK(applyWifiListEdit(current, {keep("c"), keep("a"), set("d", "pass-dddd"), keep("b2", "b")},
                          out, err));
  CHECK(ssids(out) == (std::vector<std::string>{"c", "a", "d", "b2"}));
  CHECK(out[1].password == "pass-aaaa");
  CHECK(out[2].password == "pass-dddd");
  CHECK(out[3].password == "pass-bbbb");
  // Delete = omit; empty password = open network.
  CHECK(applyWifiListEdit(current, {set("a", "")}, out, err));
  CHECK(out.size() == 1 && out[0].password.empty());
  CHECK(applyWifiListEdit(current, {}, out, err));
  CHECK(out.empty());
}

TEST(wifi_list, edit_rejects_invalid) {
  const std::vector<WifiAccount> current = {{"a", "pass-aaaa"}};
  std::vector<WifiAccount> out = {{"sentinel", ""}};
  WifiListError err;
  CHECK(!applyWifiListEdit(current, {keep("new")}, out, err));
  CHECK(err.field == "networks[0].password");
  CHECK(!applyWifiListEdit(current, {keep("a"), set("a", "12345678")}, out, err));
  CHECK(err.field == "networks[1].ssid");
  CHECK(!applyWifiListEdit(current, {set("x", "1234567")}, out, err));
  CHECK(err.field == "networks[0].password");
  CHECK(!applyWifiListEdit(current, {set(std::string(33, 'x'), "")}, out, err));
  CHECK(err.field == "networks[0].ssid");
  std::vector<WifiListEntry> many;
  for (size_t i = 0; i <= kMaxWifiNetworks; i++) many.push_back(set("n" + std::to_string(i), ""));
  CHECK(!applyWifiListEdit(current, many, out, err));
  CHECK(err.field == "networks");
  CHECK(ssids(out) == std::vector<std::string>{"sentinel"});  // unchanged on failure
}

TEST(wifi_list, plan_without_scan_walks_list_in_order) {
  const std::vector<WifiAccount> accounts = {{"a", ""}, {"", ""}, {"c", ""}};
  const auto plan = planWifiAttempts(accounts, nullptr);
  CHECK(plan.size() == 4);
  CHECK(plan[0].index == 0 && plan[0].timeout_ms == kWifiVisibleFirstTimeoutMs);
  CHECK(plan[1].index == 0 && plan[1].timeout_ms == kWifiVisibleRetryTimeoutMs);
  CHECK(plan[2].index == 2 && plan[3].index == 2);
  const std::vector<WifiScanHit> empty;
  CHECK(planWifiAttempts(accounts, &empty).size() == 4);
}

TEST(wifi_list, plan_prefers_visible_then_unseen) {
  const std::vector<WifiAccount> accounts = {{"a", ""}, {"b", ""}, {"c", ""}};
  const std::vector<WifiScanHit> scan = {{"other", -40}, {"c", -70}, {"b", -60}};
  const auto plan = planWifiAttempts(accounts, &scan);
  CHECK(plan.size() == 5);
  // Visible known networks keep list priority (b before c), two tries each.
  CHECK(plan[0].index == 1 && plan[1].index == 1);
  CHECK(plan[2].index == 2 && plan[3].index == 2);
  // Unseen (hidden / missed) networks get one short attempt afterwards.
  CHECK(plan[4].index == 0 && plan[4].timeout_ms == kWifiUnseenTimeoutMs);
}

TEST(wifi_list, reconnect_backoff_grows_caps_and_resets) {
  WifiReconnectScheduler sched(10000, 60000);
  CHECK(sched.due(0));
  sched.onOffline(0);
  sched.onAttemptFailed(1000);
  CHECK(sched.nextRetryMs() == 11000);
  CHECK(!sched.due(10999) && sched.due(11000));
  sched.onAttemptFailed(11000);  // 20s
  sched.onAttemptFailed(31000);  // 40s
  sched.onAttemptFailed(71000);  // 60s cap
  CHECK(sched.backoffMs() == 60000);
  sched.onAttemptFailed(131000);
  CHECK(sched.nextRetryMs() == 191000);  // capped, never gives up
  CHECK(sched.attempts() == 5);
  sched.onConnected(200000);
  CHECK(sched.backoffMs() == 10000);
  CHECK(sched.due(200000));
  CHECK(sched.offlineSinceMs() == -1);
}

TEST(wifi_list, reconnect_kick_retries_now) {
  WifiReconnectScheduler sched(10000, 60000);
  sched.onAttemptFailed(0);
  sched.onAttemptFailed(10000);
  sched.kick(12000);
  CHECK(sched.due(12000));
  CHECK(sched.backoffMs() == 10000);
  sched.deferUntil(13000);
  CHECK(!sched.due(12500));
}

TEST(wifi_list, driver_restart_after_five_minutes_offline) {
  WifiReconnectScheduler sched;
  CHECK(!sched.driverRestartDue(10 * 60 * 1000));  // online: never
  sched.onOffline(1000);
  sched.onOffline(50000);  // offline start sticks to first call
  CHECK(!sched.driverRestartDue(1000 + kWifiDriverRestartMs - 1));
  CHECK(sched.driverRestartDue(1000 + kWifiDriverRestartMs));
  sched.onDriverRestarted(1000 + kWifiDriverRestartMs);
  CHECK(!sched.driverRestartDue(1000 + kWifiDriverRestartMs + 1000));
  CHECK(sched.driverRestartDue(1000 + 2 * kWifiDriverRestartMs));
  sched.onConnected(0);
  CHECK(!sched.driverRestartDue(1000 + 3 * kWifiDriverRestartMs));
}

TEST(wifi_list, max_backoff_below_start_is_clamped) {
  WifiReconnectScheduler sched(10000, 1000);
  sched.onAttemptFailed(0);
  CHECK(sched.backoffMs() == 10000);
}

TEST(wifi_list, ap_fallback_waits_for_repeated_failures) {
  ApFallbackPolicy p;
  CHECK(p.due(0, 0));  // nothing saved: open the recovery AP right away
  CHECK(!p.due(0, 1));
  p.onPassFailed(1000, true);  // one failed pass with the router visible: keep trying STA only
  CHECK(!p.due(1000, 1));
  p.onPassFailed(20000, true);
  CHECK(!p.due(20000, 1));
  p.onPassFailed(50000, true);
  CHECK(p.due(50000, 1));  // third consecutive failed pass
  p.onConnected();
  CHECK(!p.due(60000, 1) && p.failedPasses() == 0);
}

TEST(wifi_list, ap_fallback_when_no_saved_network_visible) {
  ApFallbackPolicy p;
  p.onPassFailed(0, false);
  CHECK(!p.due(kApFallbackInvisibleMs - 1, 2));
  CHECK(p.due(kApFallbackInvisibleMs, 2));
  ApFallbackPolicy q;
  q.onPassFailed(0, false);
  q.onPassFailed(kApFallbackInvisibleMs - 10, true);  // became visible: timer resets
  CHECK(!q.due(kApFallbackInvisibleMs + 10, 2));
}
