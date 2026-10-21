// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "dashboard_ui/layout.h"

namespace eego::dashboard::ui {

struct PanelVersion {
  std::string id;
  uint64_t fingerprint = 0;
  Rect bounds;
  size_t order = 0;
};

struct UpdatePlan {
  std::vector<PanelVersion> current;
  DirtyRects dirty;
  size_t dirty_panels = 0;
  bool full_redraw = false;
  bool structural = false;
};

UpdatePlan planUpdate(const PageLayout& layout, const std::vector<uint64_t>& fingerprints,
                      const std::vector<PanelVersion>* previous,
                      const std::vector<std::string>& manual_dirty = {},
                      bool force_full = false);

}  // namespace eego::dashboard::ui
