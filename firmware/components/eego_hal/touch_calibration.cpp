// SPDX-License-Identifier: GPL-3.0-or-later
// 触控仿射标定：映射、最小二乘拟合、合理性校验与持久化编码（纯逻辑，见头文件）。
#include "touch_calibration.h"

#include <cmath>
#include <cstring>

namespace eego::touchcal {

namespace {

// 出厂映射：x = rawY*551/680，y = (920-rawX)*767/920，公共分母 680*920。
constexpr int32_t kDefaultDen = 680 * 920;                  // 625600
constexpr int32_t kDefaultB = 551 * 920;                    // 506920
constexpr int32_t kDefaultD = -767 * 680;                   // -521560
constexpr int32_t kDefaultF = 920 * 767 * 680;              // 479835200
constexpr double kScaleX0 = 551.0 / 680.0;                  // 逻辑 x / rawY
constexpr double kScaleY0 = 767.0 / 920.0;                  // 逻辑 y / -rawX
constexpr double kMaxScaleDeviation = 0.30;                 // 主轴缩放 ±30%
constexpr double kMaxCrossRatio = 0.25;                     // 交叉项（旋转/错切）
constexpr double kMaxCenterShiftPx = 160.0;                 // 屏幕中心平移
constexpr int32_t kFitDen = 1 << 16;
constexpr double kMinRawSpread = 60.0;                      // 各轴原始坐标标准差下限
constexpr double kMinDecorrelation = 0.1;                   // det/(Sxx*Syy) 下限
constexpr int kDuplicateRaw = 40;                           // 与上一点重合判定（raw 单位）

int64_t floorDiv(int64_t num, int64_t den) {
  int64_t q = num / den;
  if ((num % den != 0) && ((num < 0) != (den < 0))) --q;
  return q;
}

uint16_t clampAxis(int64_t v, int limit) {
  if (v < 0) return 0;
  if (v > limit - 1) return static_cast<uint16_t>(limit - 1);
  return static_cast<uint16_t>(v);
}

void put32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}

uint32_t get32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool fitsInt32(double v) { return std::isfinite(v) && v > -2147483647.0 && v < 2147483647.0; }

}  // namespace

Affine defaultAffine() {
  Affine a;
  a.a = 0;
  a.b = kDefaultB;
  a.c = 0;
  a.d = kDefaultD;
  a.e = 0;
  a.f = kDefaultF;
  a.den = kDefaultDen;
  return a;
}

bool sameAffine(const Affine& l, const Affine& r) {
  return l.a == r.a && l.b == r.b && l.c == r.c && l.d == r.d && l.e == r.e && l.f == r.f &&
         l.den == r.den;
}

bool isDefault(const Affine& affine) { return sameAffine(affine, defaultAffine()); }

bool plausible(const Affine& t) {
  if (t.den <= 0) return false;
  const double den = static_cast<double>(t.den);
  const double a = t.a / den;  // x / rawX（交叉）
  const double b = t.b / den;  // x / rawY（主轴）
  const double d = t.d / den;  // y / rawX（主轴，负）
  const double e = t.e / den;  // y / rawY（交叉）
  if (std::fabs(b / kScaleX0 - 1.0) > kMaxScaleDeviation) return false;
  if (std::fabs(-d / kScaleY0 - 1.0) > kMaxScaleDeviation) return false;
  if (std::fabs(a) > kMaxCrossRatio * kScaleX0) return false;
  if (std::fabs(e) > kMaxCrossRatio * kScaleY0) return false;
  // 出厂映射下屏幕中心对应的原始点，经本标定后不应偏离中心太远。
  const double rawY = (kLogicalW / 2) / kScaleX0;
  const double rawX = 920.0 - (kLogicalH / 2) / kScaleY0;
  const double cx = a * rawX + b * rawY + t.c / den;
  const double cy = d * rawX + e * rawY + t.f / den;
  return std::hypot(cx - kLogicalW / 2, cy - kLogicalH / 2) <= kMaxCenterShiftPx;
}

void map(const Affine& t, uint16_t rawX, uint16_t rawY, uint16_t& outX, uint16_t& outY) {
  const Affine& m = t.den > 0 ? t : defaultAffine();
  const int64_t rx = rawX;
  const int64_t ry = rawY;
  const int64_t nx = static_cast<int64_t>(m.a) * rx + static_cast<int64_t>(m.b) * ry + m.c;
  const int64_t ny = static_cast<int64_t>(m.d) * rx + static_cast<int64_t>(m.e) * ry + m.f;
  outX = clampAxis(floorDiv(nx, m.den), kLogicalW);
  outY = clampAxis(floorDiv(ny, m.den), kLogicalH);
}

const char* fitErrorName(FitError error) {
  switch (error) {
    case FitError::None: return "ok";
    case FitError::TooFewPoints: return "too-few-points";
    case FitError::Degenerate: return "degenerate";
    case FitError::Residual: return "residual";
    case FitError::Implausible: return "implausible";
  }
  return "?";
}

FitResult fit(const Pair* pairs, size_t count, double maxResidualPx) {
  FitResult result;
  if (pairs == nullptr || count < 3) {
    result.error = FitError::TooFewPoints;
    return result;
  }
  const double n = static_cast<double>(count);
  double mx = 0, my = 0, mu = 0, mv = 0;
  for (size_t i = 0; i < count; ++i) {
    mx += pairs[i].rawX;
    my += pairs[i].rawY;
    mu += pairs[i].x;
    mv += pairs[i].y;
  }
  mx /= n;
  my /= n;
  mu /= n;
  mv /= n;
  double sxx = 0, syy = 0, sxy = 0, sxu = 0, syu = 0, sxv = 0, syv = 0;
  for (size_t i = 0; i < count; ++i) {
    const double dx = pairs[i].rawX - mx;
    const double dy = pairs[i].rawY - my;
    const double du = pairs[i].x - mu;
    const double dv = pairs[i].y - mv;
    sxx += dx * dx;
    syy += dy * dy;
    sxy += dx * dy;
    sxu += dx * du;
    syu += dy * du;
    sxv += dx * dv;
    syv += dy * dv;
  }
  const double det = sxx * syy - sxy * sxy;
  if (sxx / n < kMinRawSpread * kMinRawSpread || syy / n < kMinRawSpread * kMinRawSpread ||
      det < kMinDecorrelation * sxx * syy) {
    result.error = FitError::Degenerate;
    return result;
  }
  // 中心化正规方程：[sxx sxy; sxy syy] [p q]^T = [s*u ...]
  const double a = (sxu * syy - syu * sxy) / det;
  const double b = (syu * sxx - sxu * sxy) / det;
  const double c = mu - a * mx - b * my;
  const double d = (sxv * syy - syv * sxy) / det;
  const double e = (syv * sxx - sxv * sxy) / det;
  const double f = mv - d * mx - e * my;
  // 映射用 floor：常数项 +0.5 使整数化结果为四舍五入。
  const double scale = static_cast<double>(kFitDen);
  const double coeffs[6] = {a * scale, b * scale, (c + 0.5) * scale,
                            d * scale, e * scale, (f + 0.5) * scale};
  for (double v : coeffs) {
    if (!fitsInt32(v)) {
      result.error = FitError::Implausible;
      return result;
    }
  }
  Affine t;
  t.a = static_cast<int32_t>(std::llround(coeffs[0]));
  t.b = static_cast<int32_t>(std::llround(coeffs[1]));
  t.c = static_cast<int32_t>(std::llround(coeffs[2]));
  t.d = static_cast<int32_t>(std::llround(coeffs[3]));
  t.e = static_cast<int32_t>(std::llround(coeffs[4]));
  t.f = static_cast<int32_t>(std::llround(coeffs[5]));
  t.den = kFitDen;
  result.affine = t;

  double worst = 0;
  double sum2 = 0;
  for (size_t i = 0; i < count; ++i) {
    const double rx = std::fmin(std::fmax(std::round(pairs[i].rawX), 0.0), 65535.0);
    const double ry = std::fmin(std::fmax(std::round(pairs[i].rawY), 0.0), 65535.0);
    uint16_t ox = 0, oy = 0;
    map(t, static_cast<uint16_t>(rx), static_cast<uint16_t>(ry), ox, oy);
    const double r = std::hypot(ox - pairs[i].x, oy - pairs[i].y);
    worst = std::fmax(worst, r);
    sum2 += r * r;
  }
  result.maxResidualPx = worst;
  result.rmsResidualPx = std::sqrt(sum2 / n);
  if (!plausible(t)) {
    result.error = FitError::Implausible;
    return result;
  }
  if (worst > maxResidualPx) {
    result.error = FitError::Residual;
    return result;
  }
  result.ok = true;
  result.error = FitError::None;
  return result;
}

uint32_t crc32(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

void encode(const Affine& t, uint8_t (&out)[kBlobSize]) {
  std::memset(out, 0, sizeof(out));
  put32(out, kBlobMagic);
  out[4] = kBlobVersion;
  const int32_t values[7] = {t.a, t.b, t.c, t.d, t.e, t.f, t.den};
  for (size_t i = 0; i < 7; ++i) put32(out + 8 + i * 4, static_cast<uint32_t>(values[i]));
  put32(out + kBlobSize - 4, crc32(out, kBlobSize - 4));
}

bool decode(const uint8_t* data, size_t len, Affine& out) {
  if (data == nullptr || len != kBlobSize) return false;
  if (get32(data) != kBlobMagic || data[4] != kBlobVersion) return false;
  if (get32(data + kBlobSize - 4) != crc32(data, kBlobSize - 4)) return false;
  Affine t;
  int32_t* fields[7] = {&t.a, &t.b, &t.c, &t.d, &t.e, &t.f, &t.den};
  for (size_t i = 0; i < 7; ++i) *fields[i] = static_cast<int32_t>(get32(data + 8 + i * 4));
  if (!plausible(t)) return false;
  out = t;
  return true;
}

Target target(size_t index) {
  constexpr int16_t l = kTargetInset;
  constexpr int16_t r = kLogicalW - 1 - kTargetInset;
  constexpr int16_t t = kTargetInset;
  constexpr int16_t b = kLogicalH - 1 - kTargetInset;
  switch (index) {
    case 0: return {l, t};
    case 1: return {r, t};
    case 2: return {r, b};
    case 3: return {l, b};
    default: return {kLogicalW / 2, kLogicalH / 2};
  }
}

void Session::reset() { count_ = 0; }

bool Session::add(uint16_t rawX, uint16_t rawY, uint16_t samples) {
  if (complete() || samples < 2) return false;
  if (count_ > 0) {
    const Pair& prev = pairs_[count_ - 1];
    if (std::fabs(prev.rawX - rawX) + std::fabs(prev.rawY - rawY) < kDuplicateRaw) return false;
  }
  const Target goal = target(count_);
  Pair& p = pairs_[count_++];
  p.rawX = rawX;
  p.rawY = rawY;
  p.x = goal.x;
  p.y = goal.y;
  return true;
}

FitResult Session::solve(double maxResidualPx) const {
  return fit(pairs_, count_, maxResidualPx);
}

}  // namespace eego::touchcal
