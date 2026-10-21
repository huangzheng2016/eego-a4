// SPDX-License-Identifier: GPL-3.0-or-later
#include "dashboard_ui/geometry.h"

#include <algorithm>
#include <limits>

namespace eego::dashboard::ui {

bool operator==(const Rect& lhs, const Rect& rhs) {
  return lhs.x == rhs.x && lhs.y == rhs.y && lhs.w == rhs.w && lhs.h == rhs.h;
}

Rect intersect(const Rect& lhs, const Rect& rhs) {
  const int32_t x0 = std::max<int32_t>(lhs.x, rhs.x);
  const int32_t y0 = std::max<int32_t>(lhs.y, rhs.y);
  const int32_t x1 = std::min(lhs.right(), rhs.right());
  const int32_t y1 = std::min(lhs.bottom(), rhs.bottom());
  if (x1 <= x0 || y1 <= y0) return {};
  return {static_cast<int16_t>(x0), static_cast<int16_t>(y0),
          static_cast<int16_t>(x1 - x0), static_cast<int16_t>(y1 - y0)};
}

Rect unite(const Rect& lhs, const Rect& rhs) {
  if (lhs.empty()) return rhs;
  if (rhs.empty()) return lhs;
  const int32_t x0 = std::min<int32_t>(lhs.x, rhs.x);
  const int32_t y0 = std::min<int32_t>(lhs.y, rhs.y);
  const int32_t x1 = std::max(lhs.right(), rhs.right());
  const int32_t y1 = std::max(lhs.bottom(), rhs.bottom());
  return {static_cast<int16_t>(x0), static_cast<int16_t>(y0),
          static_cast<int16_t>(x1 - x0), static_cast<int16_t>(y1 - y0)};
}

Rect inset(const Rect& rect, int16_t amount) {
  if (amount <= 0) return rect;
  const int32_t x = static_cast<int32_t>(rect.x) + amount;
  const int32_t y = static_cast<int32_t>(rect.y) + amount;
  const int32_t w = static_cast<int32_t>(rect.w) - 2 * amount;
  const int32_t h = static_cast<int32_t>(rect.h) - 2 * amount;
  if (w <= 0 || h <= 0) return {};
  return {static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(w),
          static_cast<int16_t>(h)};
}

Rect clipTo(const Rect& rect, const Rect& bounds) {
  return intersect(rect, bounds);
}

bool overlaps(const Rect& lhs, const Rect& rhs) {
  return !intersect(lhs, rhs).empty();
}

bool DirtyRects::add(const Rect& rect) {
  const Rect clipped = clipTo(rect, bounds_);
  if (clipped.empty()) return false;
  rects_.push_back(clipped);

  for (size_t i = 0; i < rects_.size();) {
    bool merged = false;
    for (size_t j = i + 1; j < rects_.size(); ++j) {
      if (overlaps(rects_[i], rects_[j])) {
        rects_[i] = unite(rects_[i], rects_[j]);
        rects_.erase(rects_.begin() + static_cast<std::ptrdiff_t>(j));
        merged = true;
        break;
      }
    }
    if (!merged) ++i;
  }

  while (rects_.size() > kMaxDirtyRects) {
    size_t best_i = 0;
    size_t best_j = 1;
    uint32_t best_area = std::numeric_limits<uint32_t>::max();
    for (size_t i = 0; i < rects_.size(); ++i) {
      for (size_t j = i + 1; j < rects_.size(); ++j) {
        const Rect candidate = unite(rects_[i], rects_[j]);
        const uint32_t area = static_cast<uint32_t>(candidate.w) * candidate.h;
        if (area < best_area) {
          best_area = area;
          best_i = i;
          best_j = j;
        }
      }
    }
    rects_[best_i] = unite(rects_[best_i], rects_[best_j]);
    rects_.erase(rects_.begin() + static_cast<std::ptrdiff_t>(best_j));
  }
  return true;
}

uint32_t DirtyRects::coverage() const {
  uint32_t total = 0;
  for (const Rect& rect : rects_) total += static_cast<uint32_t>(rect.w) * rect.h;
  return total;
}

}  // namespace eego::dashboard::ui
