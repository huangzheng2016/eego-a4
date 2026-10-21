// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dashboard_core/model.h"

namespace eego::dashboard {

class EvalValue {
 public:
  enum class Type { Null, Number, Bool, String };

  EvalValue() = default;
  static EvalValue makeNull();
  static EvalValue makeNumber(double value);
  static EvalValue makeBool(bool value);
  static EvalValue makeString(std::string value);

  Type type() const { return type_; }
  double number(double fallback = 0.0) const;
  bool boolean(bool fallback = false) const;
  const std::string& string() const;

 private:
  Type type_ = Type::Null;
  double number_ = 0.0;
  bool bool_ = false;
  std::string string_;
};

using EvalValues = std::vector<std::pair<std::string, EvalValue>>;

struct ExpressionError {
  size_t offset = 0;
  std::string message;
};

// Recursive-descent parser + tree evaluator. There is no assignment, member
// access, loop, function definition, dynamic call, or host API access.
bool validateExpressionSyntax(std::string_view expression, ExpressionError& err);
bool evaluateExpression(std::string_view expression, const EvalValues& values,
                        EvalValue& out, ExpressionError& err);
bool evaluateNumericExpression(std::string_view expression, const EvalValues& values,
                               double& out, ExpressionError& err);

// Whitelisted functions: abs, min, max, clamp, floor, ceil, round, sqrt, pow,
// isfinite, isnull, coalesce, and convert(value, from_unit, to_unit).

}  // namespace eego::dashboard
