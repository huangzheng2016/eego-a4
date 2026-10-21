#include "alert_core/file_io_stdio.h"

#include <sys/stat.h>

namespace alert_core {

bool StdioFileIo::writeAll(const std::string& path, const std::string& data) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) return false;
  const bool ok = data.empty() || std::fwrite(data.data(), 1, data.size(), f) == data.size();
  const bool closed = std::fclose(f) == 0;
  return ok && closed;
}

bool StdioFileIo::append(const std::string& path, const std::string& data) {
  std::FILE* f = std::fopen(path.c_str(), "ab");
  if (f == nullptr) return false;
  const bool ok = data.empty() || std::fwrite(data.data(), 1, data.size(), f) == data.size();
  const bool closed = std::fclose(f) == 0;
  return ok && closed;
}

bool StdioFileIo::readAll(const std::string& path, std::string& out) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return false;
  out.clear();
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    out.append(buf, n);
  }
  const bool ok = !std::ferror(f);
  std::fclose(f);
  return ok;
}

bool StdioFileIo::exists(const std::string& path) const {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0;
}

bool StdioFileIo::remove(const std::string& path) { return std::remove(path.c_str()) == 0; }

bool StdioFileIo::rename(const std::string& from, const std::string& to) {
  return std::rename(from.c_str(), to.c_str()) == 0;
}

bool StdioFileIo::fileSize(const std::string& path, size_t& out) const {
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) return false;
  out = static_cast<size_t>(st.st_size);
  return true;
}

}  // namespace alert_core
