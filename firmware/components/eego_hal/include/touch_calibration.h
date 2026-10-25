// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// touch_calibration —— GSLX680 原始坐标 → portrait 逻辑坐标的仿射标定（纯逻辑，主机单测）。
//
// 模型：6 个整数系数 + 公共分母（定点，无浮点误差）：
//   x = floor((a*rawX + b*rawY + c) / den)，y = floor((d*rawX + e*rawY + f) / den)，
//   结果钳制到 [0, 551] × [0, 767]。
// rawX / rawY 为触控帧 point0 的 12 位字（rawX = bytes[6-7]，rawY = bytes[4-5]）。
// 默认系数逐像素复现出厂硬编码映射（min(rawY,680)*551/680，(920-min(rawX,920))*767/920）。
//
// 拟合：N≥3 个 (raw, 目标) 点对的最小二乘仿射；拒绝退化（点共线/分布过窄）、
// 残差过大（默认 >15px）与和默认映射相差过远（缩放 ±30%、交叉项/旋转、平移）的结果。
//
// 持久化编码（NVS 命名空间 "touchcal"，见 touch_calibration_store.h）：
// magic + 版本 + 系数 + CRC32，解码校验失败即视为无标定（回退默认）。

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

// 出厂映射（未标定）。
Affine defaultAffine();
bool sameAffine(const Affine& lhs, const Affine& rhs);
bool isDefault(const Affine& affine);
// 结构有效（den > 0）且线性部分可逆、与默认映射相差在合理范围内。
bool plausible(const Affine& affine);

void map(const Affine& affine, uint16_t rawX, uint16_t rawY, uint16_t& outX, uint16_t& outY);

struct Pair {
  double rawX = 0;
  double rawY = 0;
  double x = 0;  // 目标逻辑坐标
  double y = 0;
};

enum class FitError : uint8_t { None = 0, TooFewPoints, Degenerate, Residual, Implausible };
const char* fitErrorName(FitError error);

struct FitResult {
  bool ok = false;
  FitError error = FitError::TooFewPoints;
  Affine affine;              // ok=false 且已解出时仍给出（诊断用）
  double maxResidualPx = 0;   // 用整数化后的系数逐点计算
  double rmsResidualPx = 0;
};

constexpr double kDefaultMaxResidualPx = 15.0;
FitResult fit(const Pair* pairs, size_t count, double maxResidualPx = kDefaultMaxResidualPx);

// ── 持久化编码 ─────────────────────────────────────────────────────────────
constexpr uint32_t kBlobMagic = 0x4C414354u;  // "TCAL"
constexpr uint8_t kBlobVersion = 1;
constexpr size_t kBlobSize = 4 + 4 + 7 * 4 + 4;  // magic, version+pad, 系数, crc32
uint32_t crc32(const uint8_t* data, size_t len);
void encode(const Affine& affine, uint8_t (&out)[kBlobSize]);
// 长度/magic/版本/CRC/合理性任一不符返回 false（out 不变）。
bool decode(const uint8_t* data, size_t len, Affine& out);

// ── 五点标定流程（纯状态；屏幕绘制见 main/touch_cal_screen）──────────────────
constexpr int kTargetInset = 48;
constexpr size_t kTargetCount = 5;
struct Target {
  int16_t x;
  int16_t y;
};
// 0..3 = 左上/右上/右下/左下（各内缩 kTargetInset），4 = 中心。
Target target(size_t index);

class Session {
 public:
  void reset();
  size_t step() const { return count_; }  // 下一个待采集的目标下标
  bool complete() const { return count_ >= kTargetCount; }
  // 记录当前目标的原始均值。samples < 2 或与上一点几乎重合（误触连击）时拒绝。
  bool add(uint16_t rawX, uint16_t rawY, uint16_t samples);
  FitResult solve(double maxResidualPx = kDefaultMaxResidualPx) const;

 private:
  Pair pairs_[kTargetCount];
  size_t count_ = 0;
};

}  // namespace eego::touchcal
