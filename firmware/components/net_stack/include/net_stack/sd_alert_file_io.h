// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

//
//   alert_core::AlertDb db(alert_io, "/sdcard/alertpanel/alerts.db");
//   store.attachDb(&db);
//
//
//

#include <cstddef>
#include <functional>
#include <mutex>
#include <string>

#include "alert_core/persistence.h"

namespace eego::net {
class SdAlertFileIo : public alert_core::IAlertFileIo {
 public:
  SdAlertFileIo() = default;
  ~SdAlertFileIo() override = default;
  SdAlertFileIo(const SdAlertFileIo&) = delete;
  SdAlertFileIo& operator=(const SdAlertFileIo&) = delete;

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

  size_t ioFailures() const {
    std::lock_guard<std::mutex> lk(mu_);
    return io_failures_;
  }

 private:
  bool readyLocked() const;

  mutable std::mutex mu_;
  mutable size_t io_failures_ = 0;
  mutable bool ready_logged_ = false;
};

}  // namespace eego::net
