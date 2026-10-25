// SPDX-License-Identifier: GPL-3.0-or-later
// dashboard_core expression evaluator, unit conversion and interpolation.
#include "dashboard_core/expression.h"
#include "dashboard_core/model.h"
#include "test_framework.h"

using namespace eego::dashboard;

namespace {

double num(const char* expr, const EvalValues& values = {}) {
  double out = -12345.0;
  ExpressionError err;
  const bool ok = evaluateNumericExpression(expr, values, out, err);
  CHECK_MSG(ok, expr << ": " << err.message);
  return out;
}

bool fails(const char* expr, const EvalValues& values = {}) {
  EvalValue out;
  ExpressionError err;
  return !evaluateExpression(expr, values, out, err);
}

EvalValue eval(const char* expr, const EvalValues& values = {}) {
  EvalValue out;
  ExpressionError err;
  CHECK_MSG(evaluateExpression(expr, values, out, err), expr << ": " << err.message);
  return out;
}

}  // namespace

TEST(expression, arithmetic_and_precedence) {
  CHECK_NEAR(num("1 + 2 * 3"), 7.0, 1e-12);
  CHECK_NEAR(num("(1 + 2) * 3"), 9.0, 1e-12);
  CHECK_NEAR(num("-2 * -3"), 6.0, 1e-12);
  CHECK_NEAR(num("10 / 4"), 2.5, 1e-12);
  CHECK_NEAR(num("10 % 4"), 2.0, 1e-12);
  CHECK_NEAR(num("2 - 3 - 4"), -5.0, 1e-12);  // left associative
  CHECK_NEAR(num("1.5e2"), 150.0, 1e-12);
}

TEST(expression, variables) {
  const EvalValues values = {{"value", EvalValue::makeNumber(42)},
                             {"limit", EvalValue::makeNumber(100)},
                             {"name", EvalValue::makeString("cpu")},
                             {"missing", EvalValue::makeNull()}};
  CHECK_NEAR(num("value / limit * 100", values), 42.0, 1e-9);
  CHECK(eval("name == 'cpu'", values).boolean());
  CHECK(eval("name != \"gpu\"", values).boolean());
  CHECK(eval("isnull(missing)", values).boolean());
  CHECK_NEAR(num("coalesce(missing, value)", values), 42.0, 1e-12);
  CHECK(fails("missing + 1", values));  // null is not a number
}

TEST(expression, functions) {
  CHECK_NEAR(num("abs(-3)"), 3.0, 1e-12);
  CHECK_NEAR(num("min(4, 2, 9)"), 2.0, 1e-12);
  CHECK_NEAR(num("max(4, 2, 9)"), 9.0, 1e-12);
  CHECK_NEAR(num("clamp(150, 0, 100)"), 100.0, 1e-12);
  CHECK_NEAR(num("clamp(-5, 0, 100)"), 0.0, 1e-12);
  CHECK_NEAR(num("floor(2.7) + ceil(2.1) + round(2.5)"), 2 + 3 + 3, 1e-12);
  CHECK_NEAR(num("sqrt(16)"), 4.0, 1e-12);
  CHECK_NEAR(num("pow(2, 10)"), 1024.0, 1e-12);
  CHECK(eval("isfinite(1)").boolean());
  CHECK(fails("clamp(1, 2)"));
  CHECK(fails("sqrt(-1)"));
  CHECK(fails("system(1)"));  // not whitelisted
}

TEST(expression, logic_and_conditional) {
  CHECK(eval("1 < 2 && 2 <= 2").boolean());
  CHECK(eval("1 > 2 || 3 >= 3").boolean());
  CHECK(eval("!(1 == 2)").boolean());
  CHECK_NEAR(num("1 > 0 ? 10 : 20"), 10.0, 1e-12);
  CHECK_NEAR(num("1 < 0 ? 10 : 20"), 20.0, 1e-12);
  // Short circuit: right side would divide by zero.
  CHECK(!eval("false && (1 / 0 > 0)").boolean());
  CHECK(eval("true || (1 / 0 > 0)").boolean());
}

TEST(expression, errors_are_reported) {
  CHECK(fails("1 / 0"));
  CHECK(fails("1 % 0"));
  CHECK(fails("pow(10, 400)"));  // non-finite
  ExpressionError err;
  CHECK(!validateExpressionSyntax("1 +", err));
  CHECK(!validateExpressionSyntax("(1", err));
  CHECK(!validateExpressionSyntax("a = 1", err));
  CHECK(validateExpressionSyntax("series.value + 1", err));  // dotted metric names are identifiers
  CHECK(!validateExpressionSyntax("a..b", err));
  CHECK(!validateExpressionSyntax("a. + 1", err));
  CHECK(!validateExpressionSyntax("'unterminated", err));
  CHECK(validateExpressionSyntax("value * 2 + clamp(x, 0, 1)", err));
  std::string deep(500, '(');
  deep += "1" + std::string(500, ')');
  CHECK(!validateExpressionSyntax(deep, err));  // bounded recursion
}

TEST(expression, unit_conversion) {
  CHECK_NEAR(num("convert(100, 'C', 'F')"), 212.0, 1e-9);
  CHECK_NEAR(num("convert(0, 'C', 'K')"), 273.15, 1e-9);
  CHECK_NEAR(num("convert(1, 'GiB', 'MiB')"), 1024.0, 1e-9);
  CHECK_NEAR(num("convert(0.5, 'ratio', 'percent')"), 50.0, 1e-9);
  CHECK_NEAR(num("convert(90, 'min', 'h')"), 1.5, 1e-9);
  CHECK(fails("convert(1, 'C', 'km')"));
  CHECK(fails("convert(1, 'parsec', 'km')"));
  double out = 0;
  CHECK(convertUnit(1500, "m", "km", out));
  CHECK_NEAR(out, 1.5, 1e-12);
  CHECK(!convertUnit(1, "c", "F", out));  // case-sensitive
}

TEST(expression, variable_interpolation) {
  const VariableValues values = {{"host", "srv-1"}, {"city", "Shanghai"}};
  std::string out, error;
  CHECK(interpolateVariables("http://${host}/w?c=${city}", values, out, error));
  CHECK_EQ(out, std::string("http://srv-1/w?c=Shanghai"));
  out.clear();
  CHECK(!interpolateVariables("${nope}", values, out, error));
  out.clear();
  CHECK(!interpolateVariables("${host", values, out, error));
  out.clear();
  CHECK(!interpolateVariables("${host}${host}", values, out, error, 6));  // byte budget
}
