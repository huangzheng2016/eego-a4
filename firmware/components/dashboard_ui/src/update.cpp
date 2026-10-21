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
  std::vector<bool> counted;
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
    if (panel_dirty) plan.dirty.add(next.bounds);
    counted.push_back(panel_dirty);
    if (panel_dirty) ++plan.dirty_panels;
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

    std::vector<std::string> previous_retained;
    std::vector<std::string> current_retained;
    for (const PanelVersion& cached : *previous) {
      const bool retained = std::any_of(plan.current.begin(), plan.current.end(),
                                        [&](const PanelVersion& panel) { return panel.id == cached.id; });
      if (retained) previous_retained.push_back(cached.id);
    }
    for (const PanelVersion& next : plan.current) {
      const bool retained = std::any_of(previous->begin(), previous->end(),
                                        [&](const PanelVersion& cached) { return cached.id == next.id; });
      if (retained) current_retained.push_back(next.id);
    }
    for (size_t current_index = 0; current_index < current_retained.size(); ++current_index) {
      const std::string& id = current_retained[current_index];
      const size_t previous_index = std::find(previous_retained.begin(), previous_retained.end(), id) -
                                    previous_retained.begin();
      bool same_predecessors = true;
      for (size_t i = 0; i < current_index; ++i) {
        if (std::find(previous_retained.begin(), previous_retained.begin() + previous_index,
                      current_retained[i]) == previous_retained.begin() + previous_index) {
          same_predecessors = false;
          break;
        }
      }
      for (size_t i = 0; i < previous_index && same_predecessors; ++i) {
        if (std::find(current_retained.begin(), current_retained.begin() + current_index,
                      previous_retained[i]) == current_retained.begin() + current_index) {
          same_predecessors = false;
        }
      }
      if (same_predecessors) continue;

      const size_t current_panel_index = std::find_if(
                                             plan.current.begin(), plan.current.end(),
                                             [&](const PanelVersion& candidate) {
                                               return candidate.id == id;
                                             }) -
                                         plan.current.begin();
      const PanelVersion& next = plan.current[current_panel_index];
      const PanelVersion& cached = *std::find_if(
          previous->begin(), previous->end(), [&](const PanelVersion& candidate) {
            return candidate.id == id;
          });
      plan.dirty.add(cached.bounds);
      plan.dirty.add(next.bounds);
      plan.structural = true;
      if (!counted[current_panel_index]) {
        ++plan.dirty_panels;
        counted[current_panel_index] = true;
      }
    }
  }
  if (plan.full_redraw) plan.dirty.add(layout.content);
  return plan;
}

}  // namespace eego::dashboard::ui
