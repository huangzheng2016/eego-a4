// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

//

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "admin/dashboard_config.h"
#include "admin/display_snapshot.h"

namespace eego::admin {

struct RenderPreviewRequest {
  std::shared_ptr<const DashboardDocument> document;  // validated candidate bundle
  std::string page_id;     // resolved visible page (= one device screen)
  bool fast = false;       // true: fast-refresh view (gray < 8 -> black, >= 8 -> white)
};

struct RenderPreviewResult {
  int status = 200;          // provider failure status: 400 (bad page) or 503
  std::string error;         // safe message for a failure
  std::string page_id;       // rendered page
  uint32_t screen = 0;       // 0-based index of the page among visible pages
  uint32_t screen_count = 0; // visible pages (= device screens)
  std::string data;          // "bundle" | "live" | "mixed" | "placeholder" | "none"
};

// Runs on app_task. Fill `rows` (kSnapshotHeight rows of `stride` bytes, top
// row first, see writePreviewBmpHeader for the 4-bpp pixel format; the buffer
// is pre-filled white). Return false and set result.status/error on failure.
using RenderPreviewProvider = std::function<bool(const RenderPreviewRequest& request,
                                                 uint8_t* rows, size_t stride,
                                                 RenderPreviewResult& result)>;

// Register (or clear with nullptr) the provider. Thread-safe.
void setRenderPreviewProvider(RenderPreviewProvider provider);
bool renderPreviewAvailable();

// One queued render (AdminCommand::preview). `bmp` is a PSRAM buffer of
// kPreviewFileBytes holding the header; pixel rows start at kPreviewHeaderBytes.
struct RenderPreviewJob {
  RenderPreviewRequest request;
  RenderPreviewResult result;
  uint8_t* bmp = nullptr;
  // Opaque "one preview at a time" slot held for the job's whole lifetime: the
  // HTTP handler may give up (timeout) while the command is still queued or
  // rendering on app_task, so the busy flag is released only when the last
  // owner of the job (handler or command queue) drops it.
  std::shared_ptr<void> busy_slot;
  RenderPreviewJob() = default;
  RenderPreviewJob(const RenderPreviewJob&) = delete;
  RenderPreviewJob& operator=(const RenderPreviewJob&) = delete;
  ~RenderPreviewJob();
};

namespace internal {
// app_task (defaultCommandHandler): run the registered provider on `job`.
bool runRenderPreviewJob(RenderPreviewJob& job, std::string& error);
}  // namespace internal

}  // namespace eego::admin
