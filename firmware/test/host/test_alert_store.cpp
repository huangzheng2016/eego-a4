// SPDX-License-Identifier: GPL-3.0-or-later
// alert_core store persistence: alerts.db compaction policy (boot + size/dead-line
// triggered) and the device input-binding defaults of the navigation model.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "admin/input_binding_model.h"
#include "alert_core/file_io_stdio.h"
#include "alert_core/persistence.h"
#include "alert_core/store.h"
#include "alert_core/type_registry.h"
#include "test_framework.h"

namespace {

std::string tempDbPath(const char* name) {
  const char* tmpdir = std::getenv("TMPDIR");
  const std::string dir = tmpdir != nullptr && *tmpdir != '\0' ? tmpdir : "/tmp";
  const std::string path = dir + "/eego_host_" + name + ".db";
  std::remove(path.c_str());
  std::remove((path + ".tmp").c_str());
  return path;
}

alert_core::AlertEvent event(const std::string& type, const std::string& message) {
  alert_core::AlertEvent ev;
  ev.type = type;
  ev.has_severity = true;
  ev.severity = alert_core::Severity::Warning;
  ev.title = "Disk usage " + type;
  ev.message = message;
  return ev;
}

// Stdio IO whose read-back of the compaction tmp file returns one flipped byte
// (simulated SD corruption), or whose streamed write fails half way.
class FaultyIo : public alert_core::StdioFileIo {
 public:
  bool corrupt_tmp_read = false;
  bool fail_tmp_write = false;
  bool readStream(const std::string& path, const ChunkSink& sink) override {
    if (!corrupt_tmp_read || path.size() < 4 || path.compare(path.size() - 4, 4, ".tmp") != 0)
      return StdioFileIo::readStream(path, sink);
    bool first = true;
    return StdioFileIo::readStream(path, [&](const char* data, size_t n) {
      std::string copy(data, n);
      if (first && !copy.empty()) copy[0] = static_cast<char>(copy[0] ^ 0x20);
      first = false;
      return sink(copy.data(), copy.size());
    });
  }
  bool writeStream(const std::string& path,
                   const std::function<bool(const ChunkSink& sink)>& producer) override {
    if (!fail_tmp_write) return StdioFileIo::writeStream(path, producer);
    size_t chunks = 0;
    return StdioFileIo::writeStream(path, [&](const ChunkSink& sink) {
      return producer([&](const char* data, size_t n) { return ++chunks < 3 && sink(data, n); });
    });
  }
};

std::string readText(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

TEST(alert_store, streamed_load_handles_chunk_splits_torn_tail_and_oversize_lines) {
  const std::string path = tempDbPath("stream_load");
  alert_core::StdioFileIo io;
  alert_core::TypeRegistry registry;
  {
    alert_core::AlertDb db(io, path);
    alert_core::AlertStore store(registry);
    store.attachDb(&db);
    const alert_core::AlertSource source{"http_push", "http_push", "test"};
    // Long messages so lines straddle the 1 KB read chunks.
    for (int i = 0; i < 30; ++i) {
      store.ingest(event("svc." + std::to_string(i), std::string(700 + i * 13, 'm')), source,
                   1700000000 + i);
    }
  }
  // An oversized garbage line in the middle, then a torn (newline-less) tail.
  io.append(path, std::string(70 * 1024, 'x') + "\n");
  {
    alert_core::AlertDb db(io, path);
    alert_core::AlertStore store(registry);
    store.attachDb(&db);
    const alert_core::AlertSource source{"http_push", "http_push", "test"};
    store.ingest(event("svc.after", "after the garbage"), source, 1700000100);
  }
  io.append(path, "{\"id\":\"torn");

  alert_core::AlertDb db(io, path);
  std::vector<alert_core::AlertRecord> records;
  size_t malformed = 0;
  CHECK_EQ(db.load(records, malformed), static_cast<size_t>(31));
  CHECK_EQ(malformed, static_cast<size_t>(2));
  CHECK_EQ(db.totalLines(), static_cast<size_t>(33));
  REQUIRE(records.size() == 31);
  CHECK_EQ(records.back().message, std::string("after the garbage"));
  CHECK_EQ(records[29].message.size(), static_cast<size_t>(700 + 29 * 13));
  std::remove(path.c_str());
}

TEST(alert_store, streamed_compaction_verifies_and_rolls_back) {
  const std::string path = tempDbPath("stream_compact");
  FaultyIo io;
  alert_core::TypeRegistry registry;
  alert_core::AlertDb db(io, path);
  alert_core::AlertStore store(registry);
  store.attachDb(&db);
  const alert_core::AlertSource source{"http_push", "http_push", "test"};
  for (int round = 0; round < 3; ++round) {
    for (int i = 0; i < 300; ++i) {
      store.ingest(event("svc." + std::to_string(i), "round " + std::to_string(round)), source,
                   1700000000 + round * 1000 + i);
    }
  }
  REQUIRE(store.size() == 300);
  const std::string original = readText(path);

  // Corrupted read-back: compaction refuses, original untouched, no tmp left.
  io.corrupt_tmp_read = true;
  std::string err;
  CHECK(!store.compactDb(err));
  CHECK(err.find("verify") != std::string::npos);
  CHECK(readText(path) == original);
  CHECK(!io.exists(path + ".tmp"));
  io.corrupt_tmp_read = false;

  // Write failing part way: same rollback.
  io.fail_tmp_write = true;
  err.clear();
  CHECK(!store.compactDb(err));
  CHECK(readText(path) == original);
  CHECK(!io.exists(path + ".tmp"));
  io.fail_tmp_write = false;

  // Healthy run: one line per live record, reloads to the same set.
  err.clear();
  CHECK_MSG(store.compactDb(err), err);
  CHECK_EQ(db.totalLines(), static_cast<size_t>(300));
  CHECK_EQ(db.deadLines(), static_cast<size_t>(0));
  alert_core::AlertDb reopened(io, path);
  std::vector<alert_core::AlertRecord> records;
  size_t malformed = 0;
  CHECK_EQ(reopened.load(records, malformed), static_cast<size_t>(300));
  CHECK_EQ(malformed, static_cast<size_t>(0));
  CHECK_EQ(reopened.deadLines(), static_cast<size_t>(0));
  REQUIRE(records.size() == 300);
  CHECK_EQ(records[0].message, std::string("round 2"));
  std::remove(path.c_str());
}

TEST(alert_store, compaction_drops_superseded_lines) {
  const std::string path = tempDbPath("compact");
  alert_core::StdioFileIo io;
  alert_core::TypeRegistry registry;
  alert_core::AlertDb db(io, path);
  alert_core::AlertStore store(registry);
  store.attachDb(&db);
  const alert_core::AlertSource source{"http_push", "http_push", "test"};
  for (int i = 0; i < 40; ++i) {
    store.ingest(event("disk.a", "tick " + std::to_string(i)), source, 1700000000 + i);
    store.ingest(event("disk.b", "tick " + std::to_string(i)), source, 1700000000 + i);
  }
  REQUIRE(store.size() == 2);
  CHECK(db.deadLines() >= 70);
  size_t before = 0;
  REQUIRE(db.fileSize(before));

  // Below both thresholds: nothing happens.
  alert_core::AlertStore::CompactPolicy relaxed;
  relaxed.max_bytes = before * 4;
  relaxed.min_dead_lines = 1000;
  std::string err;
  bool attempted = true;
  CHECK(!store.maybeCompactDb(relaxed, err, &attempted));
  CHECK(!attempted);

  // Dead lines dominate: compacts to one line per live record.
  alert_core::AlertStore::CompactPolicy policy;
  policy.max_bytes = before * 4;
  policy.min_dead_lines = 16;
  CHECK_MSG(store.maybeCompactDb(policy, err, &attempted), err);
  CHECK(attempted);
  CHECK(db.deadLines() == 0);
  CHECK(db.totalLines() == 2);
  size_t after = 0;
  REQUIRE(db.fileSize(after));
  CHECK(after < before / 10);
  CHECK(store.lastCompactBytes() == after);

  // The compacted db reloads to the same live set.
  alert_core::AlertDb reopened(io, path);
  alert_core::AlertStore restored(registry);
  CHECK(restored.loadFrom(reopened) == 2);
  CHECK(reopened.deadLines() == 0);
  std::remove(path.c_str());
}

TEST(alert_store, size_trigger_waits_for_growth_after_compaction) {
  const std::string path = tempDbPath("size");
  alert_core::StdioFileIo io;
  alert_core::TypeRegistry registry;
  alert_core::AlertDb db(io, path);
  alert_core::AlertStore store(registry);
  store.attachDb(&db);
  const alert_core::AlertSource source{"http_push", "http_push", "test"};
  for (int i = 0; i < 12; ++i) {
    store.ingest(event("svc." + std::to_string(i), "live"), source, 1700000000 + i);
  }
  alert_core::AlertStore::CompactPolicy policy;
  policy.max_bytes = 256;  // live set alone exceeds this
  policy.min_dead_lines = 100000;
  std::string err;
  bool attempted = false;
  CHECK_MSG(store.maybeCompactDb(policy, err, &attempted), err);
  CHECK(attempted);
  // Same live set: still above max_bytes but has not doubled → no churn.
  CHECK(!store.maybeCompactDb(policy, err, &attempted));
  CHECK(!attempted);
  std::remove(path.c_str());
}

TEST(alert_store, input_defaults_follow_navigation_model) {
  using eego::admin::InputBinding;
  using eego::admin::InputTrigger;
  auto target = [](const char* page, InputTrigger trigger) {
    InputBinding binding;
    REQUIRE(eego::admin::resolveInputBinding({}, page, trigger, binding));
    return binding.action.target;
  };
  CHECK(target("dashboard:main", InputTrigger::UpShort) == "page_previous");
  CHECK(target("dashboard:main", InputTrigger::DownShort) == "page_next");
  CHECK(target("settings", InputTrigger::UpLong) == "focus_previous");
  CHECK(target("detail", InputTrigger::DownLong) == "focus_next");
  CHECK(target("onboarding", InputTrigger::FrontLong) == "settings");
  CHECK(target("status", InputTrigger::FrontDouble) == "status");
  CHECK(eego::admin::inputBuiltinAllowed("home"));
  CHECK(!eego::admin::inputBuiltinAllowed("overview"));

  eego::admin::InputBindingErrors errors;
  InputBinding legacy;
  legacy.page = "overview";
  legacy.trigger = InputTrigger::UpShort;
  legacy.action.kind = eego::admin::InputActionKind::Builtin;
  legacy.action.target = "focus_next";
  CHECK(!eego::admin::validateInputBindingsSyntax({legacy}, errors));
  legacy.page = "onboarding";
  CHECK(eego::admin::validateInputBindingsSyntax({legacy}, errors));
}

// A record persisted by an older firmware (removed page name "overview") is
// rejected with a reason, and the default profile written in its place loads.
TEST(alert_store, stale_input_binding_record_falls_back_to_defaults) {
  const std::string legacy =
      R"({"schema_version":1,"revision":7,"bindings":[{"page":"overview","trigger":"up.short",)"
      R"("action":{"kind":"builtin","target":"focus_next"}}]})";
  eego::admin::InputBindingSet out;
  std::string reason;
  CHECK(!eego::admin::parseStoredInputBindings(legacy, out, reason));
  CHECK_MSG(reason.find("page") != std::string::npos, reason);
  CHECK(!eego::admin::parseStoredInputBindings("{not json", out, reason));
  CHECK(!reason.empty());

  const eego::admin::InputBindingSet defaults{};
  const std::string record = eego::admin::inputBindingsToJson(defaults).toString();
  CHECK_MSG(eego::admin::parseStoredInputBindings(record, out, reason), reason);
  CHECK(out.loaded);
  CHECK(out.bindings.empty());
  CHECK_EQ(out.revision, uint32_t{1});
}
