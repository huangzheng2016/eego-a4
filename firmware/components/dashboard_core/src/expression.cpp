// SPDX-License-Identifier: GPL-3.0-or-later
#include "dashboard_core/expression.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>

namespace eego::dashboard {
namespace {

constexpr size_t kMaxNodes = 128;
constexpr size_t kMaxDepth = 24;
constexpr size_t kMaxArguments = 8;
constexpr size_t kMaxEvalSteps = 512;
const std::string kEmptyString;

enum class NodeKind { Literal, Identifier, Unary, Binary, Conditional, Call };

struct Node {
  NodeKind kind = NodeKind::Literal;
  EvalValue literal;
  std::string text;
  std::vector<Node> children;
  size_t offset = 0;
};

bool identifierStart(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
bool identifierPart(char c) {
  return identifierStart(c) || (c >= '0' && c <= '9') || c == '.';
}
bool knownFunction(std::string_view name) {
  static constexpr std::string_view kNames[] = {
      "abs", "min", "max", "clamp", "floor", "ceil", "round", "sqrt",
      "pow", "isfinite", "isnull", "coalesce", "convert"};
  return std::find(kNames, kNames + sizeof(kNames) / sizeof(kNames[0]), name) !=
         kNames + sizeof(kNames) / sizeof(kNames[0]);
}

class Parser {
 public:
  explicit Parser(std::string_view expression) : expression_(expression) {}

  bool parse(Node& out) {
    if (expression_.empty()) return fail("expression is empty");
    if (expression_.size() > kMaxExpressionBytes) return fail("expression exceeds size limit");
    if (!parseConditional(out, 0)) return false;
    skipWhitespace();
    if (position_ != expression_.size()) return fail("unexpected token");
    return true;
  }

  const ExpressionError& error() const { return error_; }

 private:
  bool fail(const std::string& message) {
    if (error_.message.empty()) {
      error_.offset = position_;
      error_.message = message;
    }
    return false;
  }

  void skipWhitespace() {
    while (position_ < expression_.size()) {
      const char c = expression_[position_];
      if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
      ++position_;
    }
  }

  bool match(std::string_view token) {
    skipWhitespace();
    if (expression_.substr(position_, token.size()) != token) return false;
    position_ += token.size();
    return true;
  }

  bool makeNode(Node& out, NodeKind kind, size_t offset) {
    if (++nodes_ > kMaxNodes) return fail("expression exceeds node limit");
    out.kind = kind;
    out.offset = offset;
    return true;
  }

  bool parseConditional(Node& out, size_t depth) {
    if (depth > kMaxDepth) return fail("expression nesting exceeds limit");
    Node condition;
    if (!parseOr(condition, depth)) return false;
    if (!match("?")) {
      out = std::move(condition);
      return true;
    }
    const size_t offset = position_ - 1;
    Node yes;
    Node no;
    if (!parseConditional(yes, depth + 1)) return false;
    if (!match(":")) return fail("expected ':' in conditional expression");
    if (!parseConditional(no, depth + 1)) return false;
    Node result;
    if (!makeNode(result, NodeKind::Conditional, offset)) return false;
    result.children.push_back(std::move(condition));
    result.children.push_back(std::move(yes));
    result.children.push_back(std::move(no));
    out = std::move(result);
    return true;
  }

  using ParseLevel = bool (Parser::*)(Node&, size_t);

  bool parseBinary(Node& out, size_t depth, ParseLevel next,
                   std::initializer_list<std::string_view> operators) {
    Node left;
    if (!(this->*next)(left, depth)) return false;
    for (;;) {
      skipWhitespace();
      std::string_view operation;
      for (const auto candidate : operators) {
        if (expression_.substr(position_, candidate.size()) == candidate) {
          operation = candidate;
          break;
        }
      }
      if (operation.empty()) break;
      const size_t offset = position_;
      position_ += operation.size();
      Node right;
      if (!(this->*next)(right, depth)) return false;
      Node combined;
      if (!makeNode(combined, NodeKind::Binary, offset)) return false;
      combined.text = std::string(operation);
      combined.children.push_back(std::move(left));
      combined.children.push_back(std::move(right));
      left = std::move(combined);
    }
    out = std::move(left);
    return true;
  }

  bool parseOr(Node& out, size_t depth) {
    return parseBinary(out, depth, &Parser::parseAnd, {"||"});
  }
  bool parseAnd(Node& out, size_t depth) {
    return parseBinary(out, depth, &Parser::parseEquality, {"&&"});
  }
  bool parseEquality(Node& out, size_t depth) {
    return parseBinary(out, depth, &Parser::parseComparison, {"==", "!="});
  }
  bool parseComparison(Node& out, size_t depth) {
    return parseBinary(out, depth, &Parser::parseTerm, {"<=", ">=", "<", ">"});
  }
  bool parseTerm(Node& out, size_t depth) {
    return parseBinary(out, depth, &Parser::parseFactor, {"+", "-"});
  }
  bool parseFactor(Node& out, size_t depth) {
    return parseBinary(out, depth, &Parser::parseUnary, {"*", "/", "%"});
  }

  bool parseUnary(Node& out, size_t depth) {
    if (depth > kMaxDepth) return fail("expression nesting exceeds limit");
    skipWhitespace();
    if (position_ < expression_.size() &&
        (expression_[position_] == '!' || expression_[position_] == '-' ||
         expression_[position_] == '+')) {
      const size_t offset = position_;
      const char operation = expression_[position_++];
      Node operand;
      if (!parseUnary(operand, depth + 1)) return false;
      Node result;
      if (!makeNode(result, NodeKind::Unary, offset)) return false;
      result.text.assign(1, operation);
      result.children.push_back(std::move(operand));
      out = std::move(result);
      return true;
    }
    return parsePrimary(out, depth);
  }

  bool parseNumber(Node& out) {
    const size_t start = position_;
    if (position_ < expression_.size() && expression_[position_] == '0') {
      ++position_;
    } else {
      while (position_ < expression_.size() && expression_[position_] >= '0' &&
             expression_[position_] <= '9')
        ++position_;
    }
    if (position_ < expression_.size() && expression_[position_] == '.') {
      ++position_;
      if (position_ >= expression_.size() || expression_[position_] < '0' ||
          expression_[position_] > '9')
        return fail("invalid numeric literal");
      while (position_ < expression_.size() && expression_[position_] >= '0' &&
             expression_[position_] <= '9')
        ++position_;
    }
    if (position_ < expression_.size() && (expression_[position_] == 'e' || expression_[position_] == 'E')) {
      ++position_;
      if (position_ < expression_.size() &&
          (expression_[position_] == '+' || expression_[position_] == '-'))
        ++position_;
      if (position_ >= expression_.size() || expression_[position_] < '0' ||
          expression_[position_] > '9')
        return fail("invalid numeric exponent");
      while (position_ < expression_.size() && expression_[position_] >= '0' &&
             expression_[position_] <= '9')
        ++position_;
    }
    if (position_ < expression_.size() && identifierPart(expression_[position_]))
      return fail("invalid character after numeric literal");
    const std::string token(expression_.substr(start, position_ - start));
    errno = 0;
    char* end = nullptr;
    const double value = std::strtod(token.c_str(), &end);
    if (errno != 0 || end != token.c_str() + token.size() || !std::isfinite(value))
      return fail("numeric literal out of range");
    if (!makeNode(out, NodeKind::Literal, start)) return false;
    out.literal = EvalValue::makeNumber(value);
    return true;
  }

  bool parseString(Node& out) {
    const size_t start = position_;
    const char quote = expression_[position_++];
    std::string value;
    while (position_ < expression_.size()) {
      const unsigned char c = static_cast<unsigned char>(expression_[position_++]);
      if (c == static_cast<unsigned char>(quote)) {
        if (!makeNode(out, NodeKind::Literal, start)) return false;
        out.literal = EvalValue::makeString(std::move(value));
        return true;
      }
      if (c < 0x20) return fail("control character in string literal");
      if (c != '\\') {
        value.push_back(static_cast<char>(c));
      } else {
        if (position_ >= expression_.size()) return fail("truncated string escape");
        const char escaped = expression_[position_++];
        switch (escaped) {
          case '"': value.push_back('"'); break;
          case '\'': value.push_back('\''); break;
          case '\\': value.push_back('\\'); break;
          case '/': value.push_back('/'); break;
          case 'b': value.push_back('\b'); break;
          case 'f': value.push_back('\f'); break;
          case 'n': value.push_back('\n'); break;
          case 'r': value.push_back('\r'); break;
          case 't': value.push_back('\t'); break;
          default: return fail("unsupported string escape");
        }
      }
      if (value.size() > 256) return fail("string literal exceeds limit");
    }
    return fail("unterminated string literal");
  }

  bool parsePrimary(Node& out, size_t depth) {
    skipWhitespace();
    if (position_ >= expression_.size()) return fail("expected operand");
    const char c = expression_[position_];
    if (c >= '0' && c <= '9') return parseNumber(out);
    if (c == '"' || c == '\'') return parseString(out);
    if (c == '(') {
      ++position_;
      if (!parseConditional(out, depth + 1)) return false;
      if (!match(")")) return fail("expected ')'");
      return true;
    }
    if (!identifierStart(c)) return fail("expected operand");
    const size_t start = position_++;
    while (position_ < expression_.size() && identifierPart(expression_[position_])) ++position_;
    const std::string_view name = expression_.substr(start, position_ - start);
    if (name.back() == '.' || name.find("..") != std::string_view::npos)
      return fail("invalid identifier");
    skipWhitespace();
    if (position_ < expression_.size() && expression_[position_] == '(') {
      if (!knownFunction(name)) return fail("unknown function");
      ++position_;
      Node call;
      if (!makeNode(call, NodeKind::Call, start)) return false;
      call.text = std::string(name);
      skipWhitespace();
      if (position_ < expression_.size() && expression_[position_] == ')') {
        ++position_;
      } else {
        for (;;) {
          Node argument;
          if (!parseConditional(argument, depth + 1)) return false;
          call.children.push_back(std::move(argument));
          if (call.children.size() > kMaxArguments) return fail("too many function arguments");
          if (match(")")) break;
          if (!match(",")) return fail("expected ',' or ')' in function call");
        }
      }
      out = std::move(call);
      return true;
    }
    if (name == "true" || name == "false") {
      if (!makeNode(out, NodeKind::Literal, start)) return false;
      out.literal = EvalValue::makeBool(name == "true");
    } else if (name == "null") {
      if (!makeNode(out, NodeKind::Literal, start)) return false;
      out.literal = EvalValue::makeNull();
    } else {
      if (!makeNode(out, NodeKind::Identifier, start)) return false;
      out.text = std::string(name);
    }
    return true;
  }

  std::string_view expression_;
  size_t position_ = 0;
  size_t nodes_ = 0;
  ExpressionError error_;
};

class Evaluator {
 public:
  explicit Evaluator(const EvalValues& values) : values_(values) {}

  bool evaluate(const Node& node, EvalValue& out, size_t depth = 0) {
    if (++steps_ > kMaxEvalSteps) return fail(node, "expression evaluation step limit exceeded");
    if (depth > kMaxNodes) return fail(node, "expression evaluation depth exceeded");
    switch (node.kind) {
      case NodeKind::Literal:
        out = node.literal;
        return true;
      case NodeKind::Identifier:
        for (const auto& value : values_) {
          if (value.first == node.text) {
            out = value.second;
            return true;
          }
        }
        return fail(node, "unknown identifier: " + node.text);
      case NodeKind::Unary:
        return evaluateUnary(node, out, depth);
      case NodeKind::Binary:
        return evaluateBinary(node, out, depth);
      case NodeKind::Conditional:
        return evaluateConditional(node, out, depth);
      case NodeKind::Call:
        return evaluateCall(node, out, depth);
    }
    return fail(node, "invalid expression node");
  }

  const ExpressionError& error() const { return error_; }

 private:
  bool fail(const Node& node, const std::string& message) {
    if (error_.message.empty()) {
      error_.offset = node.offset;
      error_.message = message;
    }
    return false;
  }

  bool number(const Node& node, const EvalValue& value, double& out) {
    if (value.type() != EvalValue::Type::Number) return fail(node, "expected a number");
    out = value.number();
    return true;
  }

  bool boolean(const Node& node, const EvalValue& value, bool& out) {
    if (value.type() != EvalValue::Type::Bool) return fail(node, "expected a boolean");
    out = value.boolean();
    return true;
  }

  bool finite(const Node& node, double value) {
    return std::isfinite(value) || fail(node, "non-finite arithmetic result");
  }

  bool evaluateUnary(const Node& node, EvalValue& out, size_t depth) {
    EvalValue operand;
    if (!evaluate(node.children[0], operand, depth + 1)) return false;
    if (node.text == "!") {
      bool value = false;
      if (!boolean(node, operand, value)) return false;
      out = EvalValue::makeBool(!value);
      return true;
    }
    double value = 0.0;
    if (!number(node, operand, value)) return false;
    if (node.text == "-") value = -value;
    out = EvalValue::makeNumber(value);
    return finite(node, value);
  }

  bool equalValues(const EvalValue& left, const EvalValue& right) {
    if (left.type() != right.type()) return false;
    switch (left.type()) {
      case EvalValue::Type::Null: return true;
      case EvalValue::Type::Number: return left.number() == right.number();
      case EvalValue::Type::Bool: return left.boolean() == right.boolean();
      case EvalValue::Type::String: return left.string() == right.string();
    }
    return false;
  }

  bool evaluateBinary(const Node& node, EvalValue& out, size_t depth) {
    EvalValue left;
    if (!evaluate(node.children[0], left, depth + 1)) return false;
    if (node.text == "&&" || node.text == "||") {
      bool condition = false;
      if (!boolean(node, left, condition)) return false;
      if (node.text == "&&" && !condition) {
        out = EvalValue::makeBool(false);
        return true;
      }
      if (node.text == "||" && condition) {
        out = EvalValue::makeBool(true);
        return true;
      }
      EvalValue right;
      if (!evaluate(node.children[1], right, depth + 1)) return false;
      bool right_bool = false;
      if (!boolean(node, right, right_bool)) return false;
      out = EvalValue::makeBool(right_bool);
      return true;
    }
    EvalValue right;
    if (!evaluate(node.children[1], right, depth + 1)) return false;
    if (node.text == "==" || node.text == "!=") {
      const bool equal = equalValues(left, right);
      out = EvalValue::makeBool(node.text == "==" ? equal : !equal);
      return true;
    }
    double a = 0.0;
    double b = 0.0;
    if (!number(node, left, a) || !number(node, right, b)) return false;
    if (node.text == "<" || node.text == "<=" || node.text == ">" || node.text == ">=") {
      bool result = false;
      if (node.text == "<") result = a < b;
      else if (node.text == "<=") result = a <= b;
      else if (node.text == ">") result = a > b;
      else result = a >= b;
      out = EvalValue::makeBool(result);
      return true;
    }
    double result = 0.0;
    if (node.text == "+") result = a + b;
    else if (node.text == "-") result = a - b;
    else if (node.text == "*") result = a * b;
    else if (node.text == "/") {
      if (b == 0.0) return fail(node, "division by zero");
      result = a / b;
    } else if (node.text == "%") {
      if (b == 0.0) return fail(node, "modulo by zero");
      result = std::fmod(a, b);
    } else {
      return fail(node, "unknown binary operator");
    }
    if (!finite(node, result)) return false;
    out = EvalValue::makeNumber(result);
    return true;
  }

  bool evaluateConditional(const Node& node, EvalValue& out, size_t depth) {
    EvalValue condition;
    if (!evaluate(node.children[0], condition, depth + 1)) return false;
    bool selected = false;
    if (!boolean(node, condition, selected)) return false;
    return evaluate(node.children[selected ? 1 : 2], out, depth + 1);
  }

  bool numericArguments(const Node& node, std::vector<double>& numbers, size_t depth,
                        size_t minimum, size_t maximum) {
    if (node.children.size() < minimum || node.children.size() > maximum)
      return fail(node, "invalid argument count for " + node.text);
    for (const auto& child : node.children) {
      EvalValue value;
      if (!evaluate(child, value, depth + 1)) return false;
      double number_value = 0.0;
      if (!number(child, value, number_value)) return false;
      numbers.push_back(number_value);
    }
    return true;
  }

  bool evaluateCall(const Node& node, EvalValue& out, size_t depth) {
    if (node.text == "isnull" || node.text == "isfinite") {
      if (node.children.size() != 1) return fail(node, "invalid argument count for " + node.text);
      EvalValue value;
      if (!evaluate(node.children[0], value, depth + 1)) return false;
      if (node.text == "isnull") out = EvalValue::makeBool(value.type() == EvalValue::Type::Null);
      else out = EvalValue::makeBool(value.type() == EvalValue::Type::Number &&
                                     std::isfinite(value.number()));
      return true;
    }
    if (node.text == "coalesce") {
      if (node.children.empty()) return fail(node, "coalesce requires at least one argument");
      for (const auto& child : node.children) {
        if (!evaluate(child, out, depth + 1)) return false;
        if (out.type() != EvalValue::Type::Null) return true;
      }
      out = EvalValue::makeNull();
      return true;
    }
    if (node.text == "convert") {
      if (node.children.size() != 3) return fail(node, "convert requires three arguments");
      EvalValue input;
      EvalValue from;
      EvalValue to;
      if (!evaluate(node.children[0], input, depth + 1) ||
          !evaluate(node.children[1], from, depth + 1) ||
          !evaluate(node.children[2], to, depth + 1))
        return false;
      double value = 0.0;
      if (!number(node.children[0], input, value) || from.type() != EvalValue::Type::String ||
          to.type() != EvalValue::Type::String)
        return fail(node, "convert expects a number and two unit strings");
      double converted = 0.0;
      if (!convertUnit(value, from.string(), to.string(), converted))
        return fail(node, "unknown or incompatible unit conversion");
      out = EvalValue::makeNumber(converted);
      return true;
    }

    std::vector<double> values;
    size_t minimum = 1;
    size_t maximum = 1;
    if (node.text == "min" || node.text == "max" || node.text == "clamp") {
      minimum = node.text == "clamp" ? 3 : 1;
      maximum = node.text == "clamp" ? 3 : kMaxArguments;
    } else if (node.text == "pow") {
      minimum = maximum = 2;
    }
    if (!numericArguments(node, values, depth, minimum, maximum)) return false;
    double result = 0.0;
    if (node.text == "abs") result = std::fabs(values[0]);
    else if (node.text == "floor") result = std::floor(values[0]);
    else if (node.text == "ceil") result = std::ceil(values[0]);
    else if (node.text == "round") result = std::round(values[0]);
    else if (node.text == "sqrt") {
      if (values[0] < 0.0) return fail(node, "sqrt domain error");
      result = std::sqrt(values[0]);
    } else if (node.text == "pow") result = std::pow(values[0], values[1]);
    else if (node.text == "min") result = *std::min_element(values.begin(), values.end());
    else if (node.text == "max") result = *std::max_element(values.begin(), values.end());
    else if (node.text == "clamp") {
      if (values[1] > values[2]) return fail(node, "clamp minimum exceeds maximum");
      result = std::max(values[1], std::min(values[0], values[2]));
    } else {
      return fail(node, "unknown function");
    }
    if (!finite(node, result)) return false;
    out = EvalValue::makeNumber(result);
    return true;
  }

  const EvalValues& values_;
  size_t steps_ = 0;
  ExpressionError error_;
};

bool parseTree(std::string_view expression, Node& root, ExpressionError& err) {
  Parser parser(expression);
  if (parser.parse(root)) return true;
  err = parser.error();
  return false;
}

}  // namespace

EvalValue EvalValue::makeNull() { return EvalValue(); }
EvalValue EvalValue::makeNumber(double value) {
  EvalValue result;
  result.type_ = Type::Number;
  result.number_ = value;
  return result;
}
EvalValue EvalValue::makeBool(bool value) {
  EvalValue result;
  result.type_ = Type::Bool;
  result.bool_ = value;
  return result;
}
EvalValue EvalValue::makeString(std::string value) {
  EvalValue result;
  result.type_ = Type::String;
  result.string_ = std::move(value);
  return result;
}
double EvalValue::number(double fallback) const {
  return type_ == Type::Number ? number_ : fallback;
}
bool EvalValue::boolean(bool fallback) const {
  return type_ == Type::Bool ? bool_ : fallback;
}
const std::string& EvalValue::string() const {
  return type_ == Type::String ? string_ : kEmptyString;
}

bool validateExpressionSyntax(std::string_view expression, ExpressionError& err) {
  Node root;
  return parseTree(expression, root, err);
}

bool evaluateExpression(std::string_view expression, const EvalValues& values,
                        EvalValue& out, ExpressionError& err) {
  Node root;
  if (!parseTree(expression, root, err)) return false;
  Evaluator evaluator(values);
  if (evaluator.evaluate(root, out)) return true;
  err = evaluator.error();
  return false;
}

bool evaluateNumericExpression(std::string_view expression, const EvalValues& values,
                               double& out, ExpressionError& err) {
  EvalValue value;
  if (!evaluateExpression(expression, values, value, err)) return false;
  if (value.type() != EvalValue::Type::Number || !std::isfinite(value.number())) {
    err.offset = expression.size();
    err.message = "expression must produce a finite number";
    return false;
  }
  out = value.number();
  return true;
}

}  // namespace eego::dashboard
