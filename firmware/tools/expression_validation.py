#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Syntax-only port of dashboard_core's bounded expression parser."""

from __future__ import annotations

from native_numeric import parse_finite_strtod

MAX_NODES = 128
MAX_DEPTH = 24
MAX_ARGUMENTS = 8
MAX_CODEPOINTS = 512
FUNCTIONS = {"abs", "min", "max", "clamp", "floor", "ceil", "round", "sqrt", "pow",
             "isfinite", "isnull", "coalesce", "convert"}


def _identifier_start(char):
    return char == "_" or "A" <= char <= "Z" or "a" <= char <= "z"


def _identifier_part(char):
    return _identifier_start(char) or "0" <= char <= "9" or char == "."


class Parser:
    def __init__(self, expression):
        self.expression = expression
        self.position = 0
        self.nodes = 0
        self.error = ""

    def fail(self, message):
        if not self.error:
            self.error = f"{message} at expression byte {self.position}"
        return False

    def node(self):
        self.nodes += 1
        return self.nodes <= MAX_NODES or self.fail("expression exceeds node limit")

    def skip_whitespace(self):
        while self.position < len(self.expression) and self.expression[self.position] in " \t\r\n":
            self.position += 1

    def match(self, token):
        self.skip_whitespace()
        if not self.expression.startswith(token, self.position):
            return False
        self.position += len(token)
        return True

    def parse(self):
        if not self.expression:
            return self.fail("expression is empty")
        if len(self.expression) > MAX_CODEPOINTS:
            return self.fail("expression exceeds code-point limit")
        if not self.parse_conditional(0):
            return False
        self.skip_whitespace()
        if self.position != len(self.expression):
            return self.fail("unexpected token")
        return True

    def parse_conditional(self, depth):
        if depth > MAX_DEPTH:
            return self.fail("expression nesting exceeds limit")
        if not self.parse_or(depth):
            return False
        if not self.match("?"):
            return True
        if not self.parse_conditional(depth + 1):
            return False
        if not self.match(":"):
            return self.fail("expected ':' in conditional expression")
        if not self.parse_conditional(depth + 1):
            return False
        return self.node()

    def parse_binary(self, depth, next_parser, operators):
        if not next_parser(depth):
            return False
        while True:
            self.skip_whitespace()
            operation = next((candidate for candidate in operators
                              if self.expression.startswith(candidate, self.position)), "")
            if not operation:
                return True
            self.position += len(operation)
            if not next_parser(depth) or not self.node():
                return False

    def parse_or(self, depth):
        return self.parse_binary(depth, self.parse_and, ("||",))

    def parse_and(self, depth):
        return self.parse_binary(depth, self.parse_equality, ("&&",))

    def parse_equality(self, depth):
        return self.parse_binary(depth, self.parse_comparison, ("==", "!="))

    def parse_comparison(self, depth):
        return self.parse_binary(depth, self.parse_term, ("<=", ">=", "<", ">"))

    def parse_term(self, depth):
        return self.parse_binary(depth, self.parse_factor, ("+", "-"))

    def parse_factor(self, depth):
        return self.parse_binary(depth, self.parse_unary, ("*", "/", "%"))

    def parse_unary(self, depth):
        if depth > MAX_DEPTH:
            return self.fail("expression nesting exceeds limit")
        self.skip_whitespace()
        if self.position < len(self.expression) and self.expression[self.position] in "!+-":
            self.position += 1
            if not self.parse_unary(depth + 1):
                return False
            return self.node()
        return self.parse_primary(depth)

    def parse_number(self):
        start = self.position
        if self.position < len(self.expression) and self.expression[self.position] == "0":
            self.position += 1
        else:
            while self.position < len(self.expression) and (self.expression[self.position].isascii() and self.expression[self.position].isdigit()):
                self.position += 1
        if self.position < len(self.expression) and self.expression[self.position] == ".":
            self.position += 1
            if self.position >= len(self.expression) or not (self.expression[self.position].isascii() and self.expression[self.position].isdigit()):
                return self.fail("invalid numeric literal")
            while self.position < len(self.expression) and (self.expression[self.position].isascii() and self.expression[self.position].isdigit()):
                self.position += 1
        if self.position < len(self.expression) and self.expression[self.position] in "eE":
            self.position += 1
            if self.position < len(self.expression) and self.expression[self.position] in "+-":
                self.position += 1
            if self.position >= len(self.expression) or not (self.expression[self.position].isascii() and self.expression[self.position].isdigit()):
                return self.fail("invalid numeric exponent")
            while self.position < len(self.expression) and (self.expression[self.position].isascii() and self.expression[self.position].isdigit()):
                self.position += 1
        if self.position < len(self.expression) and _identifier_part(self.expression[self.position]):
            return self.fail("invalid character after numeric literal")
        accepted, _ = parse_finite_strtod(self.expression[start:self.position])
        if not accepted:
            return self.fail("numeric literal out of range")
        return self.node()

    def parse_string(self):
        quote = self.expression[self.position]
        self.position += 1
        value = ""
        while self.position < len(self.expression):
            char = self.expression[self.position]
            self.position += 1
            if char == quote:
                return self.node()
            if ord(char) < 0x20:
                return self.fail("control character in string literal")
            if char == "\\":
                if self.position >= len(self.expression):
                    return self.fail("truncated string escape")
                escaped = self.expression[self.position]
                self.position += 1
                if escaped not in ("\"", "'", "\\", "/", "b", "f", "n", "r", "t"):
                    return self.fail("unsupported string escape")
                value += {"b": "\b", "f": "\f", "n": "\n", "r": "\r", "t": "\t"}.get(escaped, escaped)
            else:
                value += char
            if len(value.encode("utf-8")) > 256:
                return self.fail("string literal exceeds limit")
        return self.fail("unterminated string literal")

    def parse_primary(self, depth):
        self.skip_whitespace()
        if self.position >= len(self.expression):
            return self.fail("expected operand")
        char = self.expression[self.position]
        if "0" <= char <= "9":
            return self.parse_number()
        if char in "\"'":
            return self.parse_string()
        if char == "(":
            self.position += 1
            if not self.parse_conditional(depth + 1):
                return False
            return self.match(")") or self.fail("expected ')'")
        if not _identifier_start(char):
            return self.fail("expected operand")
        start = self.position
        self.position += 1
        while self.position < len(self.expression) and _identifier_part(self.expression[self.position]):
            self.position += 1
        name = self.expression[start:self.position]
        if name.endswith(".") or ".." in name:
            return self.fail("invalid identifier")
        self.skip_whitespace()
        if self.position < len(self.expression) and self.expression[self.position] == "(":
            if name not in FUNCTIONS:
                return self.fail("unknown function")
            self.position += 1
            if not self.node():
                return False
            self.skip_whitespace()
            arguments = 0
            if self.position < len(self.expression) and self.expression[self.position] == ")":
                self.position += 1
                return True
            while True:
                if not self.parse_conditional(depth + 1):
                    return False
                arguments += 1
                if arguments > MAX_ARGUMENTS:
                    return self.fail("too many function arguments")
                if self.match(")"):
                    return True
                if not self.match(","):
                    return self.fail("expected ',' or ')' in function call")
        if name in ("true", "false", "null"):
            return self.node()
        return self.node()


def validate_expression_syntax(expression):
    parser = Parser(expression)
    valid = parser.parse()
    return valid, parser.error
