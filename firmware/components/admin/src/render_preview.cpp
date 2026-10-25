// SPDX-License-Identifier: GPL-3.0-or-later
// Render-preview provider registry and the app_task side of a queued job.
// The HTTP handler lives in dashboard_api.cpp (shares its body/validation code).
#include "admin/render_preview.h"

#include <cstring>
#include <mutex>

#include "esp_heap_caps.h"

namespace eego::admin {
namespace {

std::mutex g_preview_mutex;
RenderPreviewProvider g_preview_provider;

}  // namespace

void setRenderPreviewProvider(RenderPreviewProvider provider) {
  std::lock_guard<std::mutex> lock(g_preview_mutex);
  g_preview_provider = std::move(provider);
}

bool renderPreviewAvailable() {
  std::lock_guard<std::mutex> lock(g_preview_mutex);
  return static_cast<bool>(g_preview_provider);
}

RenderPreviewJob::~RenderPreviewJob() {
  if (bmp != nullptr) heap_caps_free(bmp);
}

namespace internal {

bool runRenderPreviewJob(RenderPreviewJob& job, std::string& error) {
  RenderPreviewProvider provider;
  {
    std::lock_guard<std::mutex> lock(g_preview_mutex);
    provider = g_preview_provider;
  }
  if (!provider || job.bmp == nullptr || job.request.document == nullptr) {
    job.result.status = 503;
    error = job.result.error = "render preview unavailable";
    return false;
  }
  uint8_t* rows = job.bmp + kPreviewHeaderBytes;
  std::memset(rows, 0x33, kPreviewStride * kSnapshotHeight);  // white (level 3) pixels
  if (!provider(job.request, rows, kPreviewStride, job.result)) {
    if (job.result.status == 200) job.result.status = 503;
    if (job.result.error.empty()) job.result.error = "render preview failed";
    error = job.result.error;
    return false;
  }
  return true;
}

}  // namespace internal
}  // namespace eego::admin
