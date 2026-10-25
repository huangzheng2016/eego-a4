// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Stdio-backed IAlertFileIo for host tools and on-device diagnostics.
// Pure C++ (<cstdio>) — usable in host verification and, if ever needed, on
// the device itself. The production SD-backed implementation (M14/M16) is
// expected to live with the board component instead.

#include <cstdio>
#include <string>

#include "alert_core/persistence.h"

namespace alert_core {

class StdioFileIo : public IAlertFileIo {
 public:
  bool writeAll(const std::string& path, const std::string& data) override;
  bool append(const std::string& path, const std::string& data) override;
  bool readAll(const std::string& path, std::string& out) override;
  bool exists(const std::string& path) const override;
  bool remove(const std::string& path) override;
  bool rename(const std::string& from, const std::string& to) override;
  bool fileSize(const std::string& path, size_t& out) const override;
  bool writeStream(const std::string& path,
                   const std::function<bool(const ChunkSink& sink)>& producer) override;
  bool readStream(const std::string& path, const ChunkSink& sink) override;
};

}  // namespace alert_core
