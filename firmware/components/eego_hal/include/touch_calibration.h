// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

//
//
//

#include <cstddef>
#include <cstdint>

namespace eego::touchcal {
constexpr int kLogicalW = 552;
constexpr int kLogicalH = 768;

struct Affine {
  int32_t a = 0;
  int32_t b = 0;
  int32_t c = 0;
  int32_t d = 0;
  int32_t e = 0;
  int32_t f = 0;
  int32_t den = 1;
};

Affine defaultAffine();
bool sameAffine(const Affine& lhs, const Affine& rhs);
bool isDefault(const Affine& affine);
bool plausible(const Affine& affine);

void map(const Affine& affine, uint16_t rawX, uint16_t rawY, uint16_t& outX, uint16_t& outY);

struct Pair {
  double rawX = 0;
  double rawY = 0;
  double x = 0;
  double y = 0;
};

enum class FitError : uint8_t { None = 0, TooFewPoints, Degenerate, Residual, Implausible };
const char* fitErrorName(FitError error);

struct FitResult {
  bool ok = false;
  FitError error = FitError::TooFewPoints;
  Affine affine;
  double maxResidualPx = 0;
  double rmsResidualPx = 0;
};

constexpr double kDefaultMaxResidualPx = 15.0;
FitResult fit(const Pair* pairs, size_t count, double maxResidualPx = kDefaultMaxResidualPx);

constexpr uint32_t kBlobMagic = 0x4C414354u;  // "TCAL"
constexpr uint8_t kBlobVersion = 1;
constexpr size_t kBlobSize = 4 + 4 + 7 * 4 + 4;
uint32_t crc32(const uint8_t* data, size_t len);
void encode(const Affine& affine, uint8_t (&out)[kBlobSize]);
bool decode(const uint8_t* data, size_t len, Affine& out);

constexpr int kTargetInset = 48;
constexpr size_t kTargetCount = 5;
struct Target {
  int16_t x;
  int16_t y;
};
Target target(size_t index);

class Session {
 public:
  void reset();
  size_t step() const { return count_; }
  bool complete() const { return count_ >= kTargetCount; }
  bool add(uint16_t rawX, uint16_t rawY, uint16_t samples);
  FitResult solve(double maxResidualPx = kDefaultMaxResidualPx) const;

 private:
  Pair pairs_[kTargetCount];
  size_t count_ = 0;
};

}  // namespace eego::touchcal
