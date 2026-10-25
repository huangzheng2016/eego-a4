// SPDX-License-Identifier: GPL-3.0-or-later
// SdAlertFileIo 实现 —— stdio/VFS 直读直写 /sdcard，
// eego_hal SdCard 提供挂载守卫；实例级互斥锁串行化（见头文件语义注释）。
#include "net_stack/sd_alert_file_io.h"

#include <sys/stat.h>
#include <cstdio>
#include <unistd.h>

#include "esp_log.h"
#include "atomic_file.h"
#include "sd_card.h"

namespace eego::net {

static const char* TAG = "net.alertio";

bool SdAlertFileIo::readyLocked() const {
  if (eego::SdCard::instance().ready()) {
    ready_logged_ = false;
    return true;
  }
  ++io_failures_;
  if (!ready_logged_) {
    ready_logged_ = true;
    ESP_LOGW(TAG, "SD card not mounted — alert file IO fails fast until remount");
  }
  return false;
}

// 写后落盘：fwrite → fflush（推 newlib 缓冲）→ fsync（FatFS f_sync，数据+元数据）。
// 不关闭流——由调用方统一 fclose（fclose 失败路径已关闭，双重 fclose 是 UB）。
static bool writeThrough(std::FILE* f, const std::string& data) {
  const bool ok = data.empty() || std::fwrite(data.data(), 1, data.size(), f) == data.size();
  if (!ok) return false;
  if (std::fflush(f) != 0) return false;
  return fsync(fileno(f)) == 0;
}

bool SdAlertFileIo::writeAll(const std::string& path, const std::string& data) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!readyLocked()) return false;
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    ++io_failures_;
    return false;
  }
  const bool ok = writeThrough(f, data);
  const bool closed = (std::fclose(f) == 0);
  if (!ok || !closed) {
    ++io_failures_;
    return false;
  }
  return true;
}

bool SdAlertFileIo::append(const std::string& path, const std::string& data) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!readyLocked()) return false;
  std::FILE* f = std::fopen(path.c_str(), "ab");
  if (!f) {
    ++io_failures_;
    return false;
  }
  const bool ok = writeThrough(f, data);
  const bool closed = (std::fclose(f) == 0);
  if (!ok || !closed) {
    ++io_failures_;
    return false;
  }
  return true;
}

bool SdAlertFileIo::readAll(const std::string& path, std::string& out) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!readyLocked()) return false;
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;  // 缺失文件是正常路径（首次启动），不计失败
  out.clear();
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    out.append(buf, n);
  }
  const bool ok = !std::ferror(f);
  std::fclose(f);
  if (!ok) ++io_failures_;
  return ok;
}

bool SdAlertFileIo::writeStream(const std::string& path,
                                const std::function<bool(const ChunkSink& sink)>& producer) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!readyLocked()) return false;
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    ++io_failures_;
    return false;
  }
  bool write_ok = true;
  const ChunkSink sink = [&](const char* data, size_t n) {
    if (n != 0 && std::fwrite(data, 1, n, f) != n) write_ok = false;
    return write_ok;
  };
  const bool produced = producer(sink);
  // 整个流写完后一次 fflush + fsync（逐块 fsync 在 SD 上过慢）。
  const bool synced = write_ok && std::fflush(f) == 0 && fsync(fileno(f)) == 0;
  const bool closed = (std::fclose(f) == 0);
  if (!produced || !write_ok || !synced || !closed) {
    ++io_failures_;
    return false;
  }
  return true;
}

bool SdAlertFileIo::readStream(const std::string& path, const ChunkSink& sink) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!readyLocked()) return false;
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;  // 缺失文件是正常路径（首次启动），不计失败
  char buf[1024];        // 小块读：net_task 栈上，避免堆分配
  size_t n;
  bool ok = true;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    if (!sink(buf, n)) {
      ok = false;
      break;
    }
  }
  const bool read_err = std::ferror(f) != 0;
  std::fclose(f);
  if (read_err) {
    ++io_failures_;
    ok = false;
  }
  return ok;
}

bool SdAlertFileIo::exists(const std::string& path) const {
  std::lock_guard<std::mutex> lk(mu_);
  if (!readyLocked()) return false;
  struct stat st;
  return ::stat(path.c_str(), &st) == 0;
}

bool SdAlertFileIo::remove(const std::string& path) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!readyLocked()) return false;
  if (::remove(path.c_str()) != 0) {
    ++io_failures_;
    return false;
  }
  return true;
}

bool SdAlertFileIo::rename(const std::string& from, const std::string& to) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!readyLocked()) return false;
  // FatFS f_rename 同目录原子，但目标已存在时返回 FR_EXIST（不覆盖）。
  // 统一走 eego_hal atomic_file：目标 → .bak、tmp → 目标、删 .bak，任一步失败
  // 回滚 .bak；断电后只剩 .bak 时由启动路径 fsutil::recoverFile() 恢复。
  if (!fsutil::replaceFile(from, to)) {
    ++io_failures_;
    return false;
  }
  return true;
}

bool SdAlertFileIo::fileSize(const std::string& path, size_t& out) const {
  std::lock_guard<std::mutex> lk(mu_);
  if (!readyLocked()) return false;
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) return false;
  out = static_cast<size_t>(st.st_size);
  return true;
}

}  // namespace eego::net
