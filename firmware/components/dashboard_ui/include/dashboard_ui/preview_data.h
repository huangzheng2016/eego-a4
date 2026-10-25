// SPDX-License-Identifier: GPL-3.0-or-later
// Data for an offscreen preview render of a candidate dashboard (admin
// POST /api/v1/dashboard/render-preview). Pure model code: no canvas or font.
#pragma once

#include <cstdint>

#include "dashboard_core/model.h"

namespace eego::dashboard::ui {

enum class PreviewData : uint8_t {
  Bundle,       // the bundle's own samples/events only
  Live,         // live device data only
  Mixed,        // bundle data plus live data for sources the bundle has no samples for
  Placeholder,  // nothing real: deterministic demo series (same rule as the web editor)
  None,         // no data and no panel that could show any
};
const char* previewDataName(PreviewData data);

// Completes `preview` (a validated candidate) in place:
//  1. samples/events already in the bundle are kept;
//  2. for every source of `preview` without bundle samples (events), the live
//     samples (events) with the same source id are appended (`live` may be null);
//  3. when the result still has no samples at all, every metric panel gets a
//     deterministic demo series (24 points, 5 min apart, ending at now_ms),
//     like web/src/model/series.ts demoSeries, so a draft shows its look.
PreviewData fillPreviewData(Dashboard& preview, const Dashboard* live, int64_t now_ms);

}  // namespace eego::dashboard::ui
