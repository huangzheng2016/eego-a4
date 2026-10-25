// SPDX-License-Identifier: GPL-3.0-or-later
// eego_hal atomic_file: FatFS-safe replace. An in-memory FileOps simulates
// FatFS (rename fails when the destination exists) and injects failures /
// power cuts at every step.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <unistd.h>

#include "atomic_file.h"
#include "test_framework.h"

namespace {

using eego::fsutil::FileOps;

class FatFsOps final : public FileOps {
 public:
  std::map<std::string, std::string> files;
  int fail_rename_call = -1;  // 0-based rename call index that fails
  int crash_after_ops = -1;   // "power cut": every op after this many fails
  int renames = 0;
  int ops = 0;

  bool alive() { return crash_after_ops < 0 || ops++ < crash_after_ops; }
  bool exists(const std::string& path) override { return files.count(path) != 0; }
  bool rename(const std::string& from, const std::string& to) override {
    if (!alive()) return false;
    const int call = renames++;
    if (call == fail_rename_call) return false;
    if (!files.count(from) || files.count(to)) return false;  // FatFS: FR_EXIST
    files[to] = files[from];
    files.erase(from);
    return true;
  }
  bool remove(const std::string& path) override {
    if (!alive()) return false;
    return files.erase(path) != 0;
  }
  bool writeSync(const std::string& path, const void* data, size_t size) override {
    if (!alive()) return false;
    files[path] = std::string(static_cast<const char*>(data), size);
    return true;
  }
};

}  // namespace

TEST(atomic_file, creates_missing_target) {
  FatFsOps fs;
  CHECK(eego::fsutil::atomicWriteFile("/sd/config.json", std::string_view("v1"), fs));
  CHECK_EQ(fs.files.size(), size_t{1});
  CHECK_EQ(fs.files["/sd/config.json"], std::string("v1"));
}

TEST(atomic_file, replaces_existing_target_on_fatfs) {
  FatFsOps fs;
  fs.files["/sd/config.json"] = "old";
  fs.files["/sd/config.json.bak"] = "stale";
  CHECK(eego::fsutil::atomicWriteFile("/sd/config.json", std::string_view("new"), fs));
  CHECK_EQ(fs.files["/sd/config.json"], std::string("new"));
  CHECK(!fs.exists("/sd/config.json.bak"));
  CHECK(!fs.exists("/sd/config.json.tmp"));
}

TEST(atomic_file, failed_final_rename_rolls_back) {
  FatFsOps fs;
  fs.files["/sd/a"] = "old";
  fs.fail_rename_call = 1;  // 0: a -> a.bak, 1: a.tmp -> a
  CHECK(!eego::fsutil::atomicWriteFile("/sd/a", std::string_view("new"), fs));
  CHECK_EQ(fs.files["/sd/a"], std::string("old"));
  CHECK(!fs.exists("/sd/a.bak"));
  CHECK(!fs.exists("/sd/a.tmp"));
}

TEST(atomic_file, failed_backup_rename_keeps_target) {
  FatFsOps fs;
  fs.files["/sd/a"] = "old";
  fs.fail_rename_call = 0;
  CHECK(!eego::fsutil::atomicWriteFile("/sd/a", std::string_view("new"), fs));
  CHECK_EQ(fs.files["/sd/a"], std::string("old"));
  CHECK(!fs.exists("/sd/a.tmp"));
}

TEST(atomic_file, power_cut_at_every_step_is_recoverable) {
  // Ops of a replace: writeSync, remove(stale .bak)?, rename(a->bak), rename(tmp->a), remove(bak).
  for (int cut = 0; cut <= 5; ++cut) {
    FatFsOps fs;
    fs.files["/sd/a"] = "old";
    fs.crash_after_ops = cut;
    const bool ok = eego::fsutil::atomicWriteFile("/sd/a", std::string_view("new"), fs);
    // Reboot: power is back, recover before reading.
    fs.crash_after_ops = -1;
    eego::fsutil::recoverFile("/sd/a", fs);
    REQUIRE_MSG(fs.exists("/sd/a"), "cut after " << cut << " ops");
    const std::string content = fs.files["/sd/a"];
    CHECK_MSG(content == "old" || content == "new", "cut " << cut << ": " << content);
    if (ok) CHECK_EQ(content, std::string("new"));
    // The next write always succeeds and leaves no backup behind.
    CHECK(eego::fsutil::atomicWriteFile("/sd/a", std::string_view("next"), fs));
    CHECK_EQ(fs.files["/sd/a"], std::string("next"));
    CHECK(!fs.exists("/sd/a.bak"));
  }
}

TEST(atomic_file, recover_only_when_target_missing) {
  FatFsOps fs;
  fs.files["/sd/a.bak"] = "old";
  CHECK(eego::fsutil::recoverFile("/sd/a", fs));
  CHECK_EQ(fs.files["/sd/a"], std::string("old"));
  fs.files["/sd/a.bak"] = "stale";
  CHECK(!eego::fsutil::recoverFile("/sd/a", fs));
  CHECK_EQ(fs.files["/sd/a"], std::string("old"));
  FatFsOps empty;
  CHECK(!eego::fsutil::recoverFile("/sd/none", empty));
}

TEST(atomic_file, posix_ops_round_trip) {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / ("eego_atomic_" + std::to_string(::getpid()));
  fs::create_directories(dir);
  const std::string target = (dir / "dashboard.json").string();
  CHECK(eego::fsutil::atomicWriteFile(target, std::string_view("one")));
  CHECK(eego::fsutil::atomicWriteFile(target, std::string_view("two")));
  std::ifstream in(target);
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK_EQ(text, std::string("two"));
  CHECK(!fs::exists(target + ".bak"));
  CHECK(!fs::exists(target + ".tmp"));
  fs::remove_all(dir);
}
