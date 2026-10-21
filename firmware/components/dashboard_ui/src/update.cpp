// SPDX-License-Identifier: GPL-3.0-or-later
#include "dashboard_ui/update.h"

#include <algorithm>

namespace eego::dashboard::ui {

UpdatePlan planUpdate(const PageLayout& layout, const std::vector<uint64_t>& fingerprints,
                      const std::vector<PanelVersion>* previous,
                      const std::vector<std::string>& manual_dirty, bool force_full) {
  UpdatePlan plan;
  plan.dirty = DirtyRects(layout.content);
  const bool incremental = previous != nullptr && !force_full;
  plan.full_redraw = !incremental;
  bool all_dirty = force_full;
  for (const std::string& id : manual_dirty) all_dirty = all_dirty || id == "*";

  const size_t count = std::min(layout.panels.size(), fingerprints.size());
  const bool detect_reorder = incremental && previous->size() == layout.panels.size();
  for (size_t i = 0; i < count; ++i) {
    const PanelLayout& panel = layout.panels[i];
    PanelVersion next{panel.panel->id, fingerprints[i], panel.bounds, i};
    const PanelVersion* cached = nullptr;
    if (incremental) {
      for (const PanelVersion& candidate : *previous) {
        if (candidate.id == next.id) {
          cached = &candidate;
          break;
        }
      }
    }
    bool panel_dirty = !incremental || all_dirty || cached == nullptr ||
                       cached->fingerprint != next.fingerprint;
    for (const std::string& id : manual_dirty) panel_dirty = panel_dirty || id == next.id;
    if (cached == nullptr && incremental) plan.structural = true;
    if (cached != nullptr && !(cached->bounds == next.bounds)) {
      plan.dirty.add(cached->bounds);
      panel_dirty = true;
      plan.structural = true;
    }
    if (cached != nullptr && detect_reorder && cached->order != next.order) {
      plan.dirty.add(cached->bounds);
      panel_dirty = true;
      plan.structural = true;
    }
    if (panel_dirty) {
      plan.dirty.add(next.bounds);
      ++plan.dirty_panels;
    }
    plan.current.push_back(std::move(next));
  }

  if (incremental) {
    for (const PanelVersion& cached : *previous) {
      const bool present = std::any_of(plan.current.begin(), plan.current.end(),
                                       [&](const PanelVersion& panel) { return panel.id == cached.id; });
      if (!present) {
        plan.dirty.add(cached.bounds);
        plan.structural = true;
        ++plan.dirty_panels;
      }
    }
  }
  if (plan.full_redraw) plan.dirty.add(layout.content);
  return plan;
}

}  // namespace eego::dashboard::ui
