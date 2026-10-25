// SPDX-License-Identifier: GPL-3.0-or-later
// eego_hal touch_calibration: affine mapping, least-squares fit, validation, blob codec.
#include <cmath>
#include <cstring>
#include <vector>

#include "eego_pins.h"
#include "test_framework.h"
#include "touch_calibration.h"

using namespace eego;
using namespace eego::touchcal;

namespace {

// Inverse of an ideal (float) affine raw→logical model, used to synthesise raw taps.
struct Model {
  double sx;   // logical x per rawY
  double sy;   // logical y per -rawX
  double ox;   // logical x offset
  double oy;   // logical y offset
  double rot;  // small cross term (logical x per rawX)
  // logical = (rot*rawX + sx*rawY + ox, oy - sy*rawX)
  void raw(double lx, double ly, double& rx, double& ry) const {
    rx = (oy - ly) / sy;
    ry = (lx - ox - rot * rx) / sx;
  }
};

std::vector<Pair> pairsFor(const Model& m, double jitter = 0.0) {
  std::vector<Pair> out;
  for (size_t i = 0; i < kTargetCount; ++i) {
    const Target t = target(i);
    Pair p;
    m.raw(t.x, t.y, p.rawX, p.rawY);
    // deterministic +/- jitter pattern in raw units
    p.rawX = std::round(p.rawX + ((i % 2) ? jitter : -jitter));
    p.rawY = std::round(p.rawY + ((i % 3) ? jitter : -jitter));
    p.x = t.x;
    p.y = t.y;
    out.push_back(p);
  }
  return out;
}

}  // namespace

TEST(touch_cal, default_matches_legacy_mapping_exactly) {
  const Affine def = defaultAffine();
  CHECK(isDefault(def));
  CHECK(plausible(def));
  for (int rx = 0; rx <= 4095; rx += 1) {
    for (int ry = 0; ry <= 4095; ry += (rx % 97 == 0 ? 1 : 37)) {
      uint16_t x = 0, y = 0;
      map(def, static_cast<uint16_t>(rx), static_cast<uint16_t>(ry), x, y);
      const uint16_t lx = rawTouchToLogicalX(static_cast<uint16_t>(ry));
      const uint16_t ly = rawTouchToLogicalY(static_cast<uint16_t>(rx));
      if (x != lx || y != ly) {
        CHECK_MSG(false, "raw " << rx << "," << ry << " -> " << x << "," << y << " legacy "
                                << lx << "," << ly);
        return;
      }
    }
  }
}

TEST(touch_cal, map_clamps_to_screen) {
  Affine t = defaultAffine();
  t.c = -100 * t.den;  // shift left 100px
  uint16_t x = 1, y = 1;
  map(t, 920, 0, x, y);
  CHECK_EQ(x, 0);
  CHECK_EQ(y, 0);
  map(t, 0, 4095, x, y);
  CHECK_EQ(x, 551);
  CHECK_EQ(y, 767);
  Affine broken;
  broken.den = 0;  // invalid → falls back to default
  map(broken, 460, 340, x, y);
  CHECK_EQ(x, rawTouchToLogicalX(340));
  CHECK_EQ(y, rawTouchToLogicalY(460));
}

TEST(touch_cal, fit_recovers_default_from_exact_points) {
  std::vector<Pair> pairs;
  for (size_t i = 0; i < kTargetCount; ++i) {
    const Target t = target(i);
    Pair p;
    p.x = t.x;
    p.y = t.y;
    p.rawY = t.x * 680.0 / 551.0;
    p.rawX = 920.0 - t.y * 920.0 / 767.0;
    pairs.push_back(p);
  }
  const FitResult r = fit(pairs.data(), pairs.size());
  REQUIRE(r.ok);
  CHECK(r.maxResidualPx <= 1.0);
  // fitted mapping agrees with default within a pixel across the panel
  for (int rx = 0; rx <= 920; rx += 23) {
    for (int ry = 0; ry <= 680; ry += 17) {
      uint16_t fx, fy, dx, dy;
      map(r.affine, static_cast<uint16_t>(rx), static_cast<uint16_t>(ry), fx, fy);
      map(defaultAffine(), static_cast<uint16_t>(rx), static_cast<uint16_t>(ry), dx, dy);
      CHECK(std::abs(fx - dx) <= 1);
      CHECK(std::abs(fy - dy) <= 1);
    }
  }
}

TEST(touch_cal, fit_corrects_vertical_offset) {
  // Real panel reads 30px too low with the factory mapping: the true mapping has oy-30.
  const Model truth{551.0 / 680.0, 767.0 / 920.0, 0.0, 767.0 - 30.0, 0.0};
  const std::vector<Pair> pairs = pairsFor(truth, 2.0);
  const FitResult r = fit(pairs.data(), pairs.size());
  REQUIRE(r.ok);
  CHECK(r.maxResidualPx <= 4.0);
  // A tap at logical (276, 400) under the true model maps back to ~(276, 400).
  double rx, ry;
  truth.raw(276, 400, rx, ry);
  uint16_t x, y, dx, dy;
  map(r.affine, static_cast<uint16_t>(std::lround(rx)), static_cast<uint16_t>(std::lround(ry)), x,
      y);
  CHECK(std::abs(x - 276) <= 2);
  CHECK(std::abs(y - 400) <= 2);
  // ...whereas the factory mapping puts it ~30px lower (the reported bug).
  map(defaultAffine(), static_cast<uint16_t>(std::lround(rx)),
      static_cast<uint16_t>(std::lround(ry)), dx, dy);
  CHECK(dy >= 425);
}

TEST(touch_cal, fit_handles_scale_and_small_rotation) {
  const Model truth{0.87, 0.80, -12.0, 760.0, 0.03};
  const std::vector<Pair> pairs = pairsFor(truth, 1.0);
  const FitResult r = fit(pairs.data(), pairs.size());
  REQUIRE(r.ok);
  CHECK(r.rmsResidualPx <= 3.0);
}

TEST(touch_cal, fit_rejects_too_few_points) {
  std::vector<Pair> pairs = pairsFor({551.0 / 680.0, 767.0 / 920.0, 0, 767, 0});
  const FitResult r = fit(pairs.data(), 2);
  CHECK(!r.ok);
  CHECK_EQ(r.error, FitError::TooFewPoints);
  CHECK(!fit(nullptr, 5).ok);
}

TEST(touch_cal, fit_rejects_degenerate_collinear_points) {
  std::vector<Pair> pairs;
  for (int i = 0; i < 5; ++i) {
    Pair p;
    p.rawX = 100 + i * 150;
    p.rawY = 100 + i * 110;  // all on one line
    p.x = 48 + i * 100;
    p.y = 48 + i * 150;
    pairs.push_back(p);
  }
  const FitResult r = fit(pairs.data(), pairs.size());
  CHECK(!r.ok);
  CHECK_EQ(r.error, FitError::Degenerate);
  // all taps in one spot
  for (Pair& p : pairs) {
    p.rawX = 400;
    p.rawY = 300;
  }
  CHECK_EQ(fit(pairs.data(), pairs.size()).error, FitError::Degenerate);
}

TEST(touch_cal, fit_rejects_large_residual) {
  std::vector<Pair> pairs = pairsFor({551.0 / 680.0, 767.0 / 920.0, 0, 767, 0});
  pairs[4].rawX += 60;  // center tap ~50px off
  const FitResult r = fit(pairs.data(), pairs.size());
  CHECK(!r.ok);
  CHECK_EQ(r.error, FitError::Residual);
  CHECK(r.maxResidualPx > 15.0);
}

TEST(touch_cal, fit_rejects_implausible_scale_or_swapped_axes) {
  // Half scale: user tapped every target at a scaled-down location.
  const std::vector<Pair> half = pairsFor({551.0 / 680.0 * 2.0, 767.0 / 920.0 * 2.0, 0, 767, 0});
  const FitResult r = fit(half.data(), half.size());
  CHECK(!r.ok);
  CHECK_EQ(r.error, FitError::Implausible);
  // Targets tapped in mirrored order (left/right swapped).
  std::vector<Pair> mirrored = pairsFor({551.0 / 680.0, 767.0 / 920.0, 0, 767, 0});
  for (Pair& p : mirrored) p.x = 551 - p.x;
  const FitResult m = fit(mirrored.data(), mirrored.size());
  CHECK(!m.ok);
  CHECK_EQ(m.error, FitError::Implausible);
}

TEST(touch_cal, blob_roundtrip_and_corruption) {
  const std::vector<Pair> pairs = pairsFor({551.0 / 680.0, 767.0 / 920.0, 4.0, 750.0, 0.01});
  const FitResult r = fit(pairs.data(), pairs.size());
  REQUIRE(r.ok);
  uint8_t blob[kBlobSize];
  encode(r.affine, blob);
  Affine out;
  REQUIRE(decode(blob, sizeof(blob), out));
  CHECK(sameAffine(out, r.affine));

  Affine untouched = defaultAffine();
  uint8_t bad[kBlobSize];
  std::memcpy(bad, blob, sizeof(bad));
  bad[12] ^= 0x01;  // coefficient bit flip → CRC mismatch
  CHECK(!decode(bad, sizeof(bad), untouched));
  CHECK(isDefault(untouched));
  std::memcpy(bad, blob, sizeof(bad));
  bad[4] = kBlobVersion + 1;  // unknown version
  CHECK(!decode(bad, sizeof(bad), untouched));
  CHECK(!decode(blob, sizeof(blob) - 1, untouched));
  CHECK(!decode(nullptr, sizeof(blob), untouched));

  // CRC-valid but implausible coefficients are rejected too.
  Affine wild = defaultAffine();
  wild.b *= 3;
  encode(wild, blob);
  CHECK(!decode(blob, sizeof(blob), untouched));
  CHECK_EQ(crc32(reinterpret_cast<const uint8_t*>("123456789"), 9), 0xCBF43926u);
}

TEST(touch_cal, targets_inset_and_center) {
  CHECK_EQ(target(0).x, 48);
  CHECK_EQ(target(0).y, 48);
  CHECK_EQ(target(2).x, 551 - 48);
  CHECK_EQ(target(2).y, 767 - 48);
  CHECK_EQ(target(4).x, 276);
  CHECK_EQ(target(4).y, 384);
}

TEST(touch_cal, session_collects_five_points_and_rejects_repeats) {
  const Model truth{551.0 / 680.0, 767.0 / 920.0, 0.0, 767.0 - 25.0, 0.0};
  Session s;
  CHECK_EQ(s.step(), 0u);
  double rx, ry;
  truth.raw(target(0).x, target(0).y, rx, ry);
  CHECK(!s.add(static_cast<uint16_t>(rx), static_cast<uint16_t>(ry), 1));  // too few samples
  CHECK(s.add(static_cast<uint16_t>(rx), static_cast<uint16_t>(ry), 6));
  // accidental second tap at the same spot is ignored
  CHECK(!s.add(static_cast<uint16_t>(rx + 5), static_cast<uint16_t>(ry - 5), 6));
  CHECK_EQ(s.step(), 1u);
  for (size_t i = 1; i < kTargetCount; ++i) {
    truth.raw(target(i).x, target(i).y, rx, ry);
    CHECK(s.add(static_cast<uint16_t>(std::lround(rx)), static_cast<uint16_t>(std::lround(ry)), 5));
  }
  CHECK(s.complete());
  CHECK(!s.add(10, 10, 5));
  const FitResult r = s.solve();
  REQUIRE(r.ok);
  CHECK(!isDefault(r.affine));
  s.reset();
  CHECK_EQ(s.step(), 0u);
  CHECK(!s.solve().ok);
}
