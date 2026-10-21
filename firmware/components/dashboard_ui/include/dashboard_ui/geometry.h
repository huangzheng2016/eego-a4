// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace eego::dashboard::ui {

struct Rect {
  int16_t x = 0;
  int16_t y = 0;
  int16_t w = 0;
  int16_t h = 0;

  bool empty() const { return w <= 0 || h <= 0; }
  int32_t right() const { return static_cast<int32_t>(x) + w; }
  int32_t bottom() const { return static_cast<int32_t>(y) + h; }
  bool contains(int16_t px, int16_t py) const {
    return !empty() && px >= x && py >= y && px < right() && py < bottom();
  }
};

inline constexpr Rect kDashboardContent{28, 28, 496, 712};
inline constexpr size_t kMaxDirtyRects = 32;

bool operator==(const Rect& lhs, const Rect& rhs);
Rect intersect(const Rect& lhs, const Rect& rhs);
Rect unite(const Rect& lhs, const Rect& rhs);
Rect inset(const Rect& rect, int16_t amount);
Rect clipTo(const Rect& rect, const Rect& bounds);
bool overlaps(const Rect& lhs, const Rect& rhs);

// Regions are clipped, coalesced, and capped. If the cap is reached, the pair
// with the smallest enclosing rectangle is merged; no dirty area is discarded.
class DirtyRects {
 public:
  explicit DirtyRects(const Rect& bounds = kDashboardContent) : bounds_(bounds) {}

  bool add(const Rect& rect);
  void clear() { rects_.clear(); }
  bool empty() const { return rects_.empty(); }
  size_t size() const { return rects_.size(); }
  const std::vector<Rect>& values() const { return rects_; }
  uint32_t coverage() const;

 private:
  Rect bounds_;
  std::vector<Rect> rects_;
};

}  // namespace eego::dashboard::ui
