// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// POST /api/v1/dashboard/render-preview —— 候选 bundle 的设备端离屏渲染预览。
//
// httpd 只解析/校验（与 /dashboard/validate 同一 DashboardStore::parse），然后经
// AdminCommand 队列（RenderPreview）把任务交给 app_task（UI 唯一属主）；app_task 调用
// app 侧注册的 provider，用真实 dashboard_ui 渲染器 + 字体画到独立的离屏
// framebuffer（绝不碰实屏 framebuffer），再按 4-bpp 行写入 admin 在 PSRAM 分配的
// BMP 缓冲。任务对象由 shared_ptr 持有：httpd 等待超时后 app_task 仍可安全写完。
// admin 不依赖 UI 头：渲染实现位于 main（见 app_main.cpp setRenderPreviewProvider）。

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
  std::string page_id;     // empty = default page (else first visible page)
  bool has_screen = false;
  uint32_t screen = 0;     // 0-based screen ("subpage") of the page
  bool fast = false;       // true: fast-refresh view (gray < 8 -> black, >= 8 -> white)
};

struct RenderPreviewResult {
  int status = 200;          // provider failure status: 400 (bad page/screen) or 503
  std::string error;         // safe message for a failure
  std::string page_id;       // rendered page
  uint32_t screen = 0;       // rendered screen
  uint32_t screen_count = 0; // screens of that page
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
