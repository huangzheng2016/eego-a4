// Client-side mirror of dashboard_core/src/expression.cpp: the safe expression
// language used by alert rules (`value > 80`) and metric mappings. Used for
// instant syntax feedback and previews; the device remains authoritative.
//
// Grammar: cond := or ('?' cond ':' cond)?; or := and ('||' and)*;
// and := eq ('&&' eq)*; eq := cmp (('=='|'!=') cmp)*; cmp := term (('<='|'>='|'<'|'>') term)*;
// term := factor (('+'|'-') factor)*; factor := unary (('*'|'/'|'%') unary)*;
// unary := ('!'|'-'|'+') unary | primary; primary := number | string | '(' cond ')' | ident ['(' args ')'].

export type EvalValue = number | boolean | string | null;

export const EXPRESSION_FUNCTIONS = ['abs', 'min', 'max', 'clamp', 'floor', 'ceil', 'round', 'sqrt', 'pow', 'isfinite', 'isnull', 'coalesce', 'convert'] as const;
const KNOWN = new Set<string>(EXPRESSION_FUNCTIONS);
const MAX_LEN = 512;
const MAX_NODES = 128;
const MAX_DEPTH = 24;
const MAX_ARGS = 8;

type Node =
  | { k: 'lit'; v: EvalValue; at: number }
  | { k: 'id'; name: string; at: number }
  | { k: 'un'; op: string; a: Node; at: number }
  | { k: 'bin'; op: string; a: Node; b: Node; at: number }
  | { k: 'cond'; c: Node; a: Node; b: Node; at: number }
  | { k: 'call'; name: string; args: Node[]; at: number };

export class ExpressionError extends Error {
  constructor(message: string, public offset: number) {
    super(message);
  }
}

const isStart = (c: string) => /[A-Za-z_]/.test(c);
const isPart = (c: string) => /[A-Za-z0-9_.]/.test(c);

class Parser {
  pos = 0;
  nodes = 0;
  constructor(private s: string) {}
  fail(msg: string): never {
    throw new ExpressionError(msg, this.pos);
  }
  ws() {
    while (this.pos < this.s.length && /\s/.test(this.s[this.pos])) this.pos++;
  }
  match(tok: string): boolean {
    this.ws();
    if (this.s.startsWith(tok, this.pos)) {
      this.pos += tok.length;
      return true;
    }
    return false;
  }
  node<T extends Node>(n: T): T {
    if (++this.nodes > MAX_NODES) this.fail('expression exceeds node limit');
    return n;
  }
  parse(): Node {
    if (!this.s.trim()) this.fail('expression is empty');
    if ([...this.s].length > MAX_LEN) this.fail('expression exceeds code-point limit');
    const n = this.cond(0);
    this.ws();
    if (this.pos !== this.s.length) this.fail('unexpected token');
    return n;
  }
  cond(d: number): Node {
    if (d > MAX_DEPTH) this.fail('expression nesting exceeds limit');
    const c = this.bin(0, d);
    this.ws();
    const at = this.pos;
    if (!this.match('?')) return c;
    const a = this.cond(d + 1);
    if (!this.match(':')) this.fail("expected ':' in conditional expression");
    const b = this.cond(d + 1);
    return this.node({ k: 'cond', c, a, b, at });
  }
  static LEVELS = [['||'], ['&&'], ['==', '!='], ['<=', '>=', '<', '>'], ['+', '-'], ['*', '/', '%']];
  bin(level: number, d: number): Node {
    if (level >= Parser.LEVELS.length) return this.unary(d);
    let left = this.bin(level + 1, d);
    for (;;) {
      this.ws();
      const op = Parser.LEVELS[level].find((o) => this.s.startsWith(o, this.pos));
      if (!op) break;
      const at = this.pos;
      this.pos += op.length;
      const right = this.bin(level + 1, d);
      left = this.node({ k: 'bin', op, a: left, b: right, at });
    }
    return left;
  }
  unary(d: number): Node {
    if (d > MAX_DEPTH) this.fail('expression nesting exceeds limit');
    this.ws();
    const c = this.s[this.pos];
    if (c === '!' || c === '-' || c === '+') {
      const at = this.pos++;
      return this.node({ k: 'un', op: c, a: this.unary(d + 1), at });
    }
    return this.primary(d);
  }
  primary(d: number): Node {
    this.ws();
    if (this.pos >= this.s.length) this.fail('expected operand');
    const c = this.s[this.pos];
    const at = this.pos;
    if (/[0-9.]/.test(c)) {
      const m = /^(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?/.exec(this.s.slice(this.pos));
      if (!m) this.fail('invalid number');
      this.pos += m[0].length;
      if (this.pos < this.s.length && isPart(this.s[this.pos])) this.fail('invalid number');
      return this.node({ k: 'lit', v: Number(m[0]), at });
    }
    if (c === '"' || c === "'") {
      this.pos++;
      let out = '';
      while (this.pos < this.s.length) {
        const ch = this.s[this.pos++];
        if (ch === c) return this.node({ k: 'lit', v: out, at });
        if (ch === '\\') {
          if (this.pos >= this.s.length) this.fail('truncated string escape');
          const e = this.s[this.pos++];
          out += e === 'n' ? '\n' : e === 't' ? '\t' : e;
        } else out += ch;
      }
      this.fail('unterminated string');
    }
    if (c === '(') {
      this.pos++;
      const n = this.cond(d + 1);
      if (!this.match(')')) this.fail("expected ')'");
      return n;
    }
    if (!isStart(c)) this.fail('expected operand');
    let end = this.pos + 1;
    while (end < this.s.length && isPart(this.s[end])) end++;
    const name = this.s.slice(this.pos, end);
    this.pos = end;
    if (name.endsWith('.') || name.includes('..')) this.fail('invalid identifier');
    this.ws();
    if (this.s[this.pos] === '(') {
      if (!KNOWN.has(name)) {
        this.pos = at;
        this.fail('unknown function');
      }
      this.pos++;
      const args: Node[] = [];
      this.ws();
      if (this.s[this.pos] === ')') this.pos++;
      else
        for (;;) {
          args.push(this.cond(d + 1));
          if (args.length > MAX_ARGS) this.fail('too many function arguments');
          if (this.match(')')) break;
          if (!this.match(',')) this.fail("expected ',' or ')' in function call");
        }
      return this.node({ k: 'call', name, args, at });
    }
    if (name === 'true' || name === 'false') return this.node({ k: 'lit', v: name === 'true', at });
    if (name === 'null') return this.node({ k: 'lit', v: null, at });
    return this.node({ k: 'id', name, at });
  }
}

export function parseExpression(s: string): Node {
  return new Parser(s).parse();
}

/** Null when the syntax is valid, else the error. */
export function checkExpression(s: string): ExpressionError | null {
  try {
    parseExpression(s);
    return null;
  } catch (e) {
    return e instanceof ExpressionError ? e : new ExpressionError(String(e), 0);
  }
}

/** Identifiers referenced by an expression (for "unknown variable" hints). */
export function identifiers(s: string): string[] {
  const out = new Set<string>();
  const walk = (n: Node) => {
    if (n.k === 'id') out.add(n.name);
    else if (n.k === 'un') walk(n.a);
    else if (n.k === 'bin') [n.a, n.b].forEach(walk);
    else if (n.k === 'cond') [n.c, n.a, n.b].forEach(walk);
    else if (n.k === 'call') n.args.forEach(walk);
  };
  try {
    walk(parseExpression(s));
  } catch {
    /* invalid */
  }
  return [...out];
}

function ev(n: Node, vars: Record<string, EvalValue>): EvalValue {
  const fail = (m: string): never => {
    throw new ExpressionError(m, n.at);
  };
  const num = (v: EvalValue): number => (typeof v === 'number' ? v : fail('expected a number'));
  const bool = (v: EvalValue): boolean => (typeof v === 'boolean' ? v : fail('expected a boolean'));
  const fin = (v: number) => (Number.isFinite(v) ? v : fail('non-finite arithmetic result'));
  switch (n.k) {
    case 'lit':
      return n.v;
    case 'id':
      return n.name in vars ? vars[n.name] : fail(`unknown identifier: ${n.name}`);
    case 'un': {
      const v = ev(n.a, vars);
      if (n.op === '!') return !bool(v);
      return fin(n.op === '-' ? -num(v) : num(v));
    }
    case 'cond':
      return bool(ev(n.c, vars)) ? ev(n.a, vars) : ev(n.b, vars);
    case 'bin': {
      const a = ev(n.a, vars);
      if (n.op === '&&') return bool(a) ? bool(ev(n.b, vars)) : false;
      if (n.op === '||') return bool(a) ? true : bool(ev(n.b, vars));
      const b = ev(n.b, vars);
      if (n.op === '==') return a === b;
      if (n.op === '!=') return a !== b;
      const x = num(a);
      const y = num(b);
      switch (n.op) {
        case '<': return x < y;
        case '<=': return x <= y;
        case '>': return x > y;
        case '>=': return x >= y;
        case '+': return fin(x + y);
        case '-': return fin(x - y);
        case '*': return fin(x * y);
        case '/': return y === 0 ? fail('division by zero') : fin(x / y);
        case '%': return y === 0 ? fail('modulo by zero') : fin(x % y);
      }
      return fail('unknown binary operator');
    }
    case 'call': {
      const args = n.args;
      const count = (lo: number, hi: number) => (args.length < lo || args.length > hi ? fail(`invalid argument count for ${n.name}`) : 0);
      if (n.name === 'isnull' || n.name === 'isfinite') {
        count(1, 1);
        const v = ev(args[0], vars);
        return n.name === 'isnull' ? v === null : typeof v === 'number' && Number.isFinite(v);
      }
      if (n.name === 'coalesce') {
        if (!args.length) fail('coalesce requires at least one argument');
        for (const a of args) {
          const v = ev(a, vars);
          if (v !== null) return v;
        }
        return null;
      }
      if (n.name === 'convert') {
        count(3, 3);
        const v = num(ev(args[0], vars));
        const from = ev(args[1], vars);
        const to = ev(args[2], vars);
        if (typeof from !== 'string' || typeof to !== 'string') fail('convert expects a number and two unit strings');
        if (from === to) return v;
        return fail('unit conversion is evaluated on the device');
      }
      const xs = args.map((a) => num(ev(a, vars)));
      switch (n.name) {
        case 'abs': count(1, 1); return Math.abs(xs[0]);
        case 'floor': count(1, 1); return Math.floor(xs[0]);
        case 'ceil': count(1, 1); return Math.ceil(xs[0]);
        case 'round': count(1, 1); return Math.round(xs[0]);
        case 'sqrt': count(1, 1); return xs[0] < 0 ? fail('sqrt of negative number') : Math.sqrt(xs[0]);
        case 'pow': count(2, 2); return fin(Math.pow(xs[0], xs[1]));
        case 'min': count(1, MAX_ARGS); return Math.min(...xs);
        case 'max': count(1, MAX_ARGS); return Math.max(...xs);
        case 'clamp': count(3, 3); return Math.min(Math.max(xs[0], xs[1]), xs[2]);
      }
      return fail('unknown function');
    }
  }
}

export type EvalResult = { ok: true; value: EvalValue } | { ok: false; error: string; offset: number };

export function evaluateExpression(s: string, vars: Record<string, EvalValue>): EvalResult {
  try {
    return { ok: true, value: ev(parseExpression(s), vars) };
  } catch (e) {
    if (e instanceof ExpressionError) return { ok: false, error: e.message, offset: e.offset };
    return { ok: false, error: String(e), offset: 0 };
  }
}

/** Alert-rule semantics (dashboard_runtime evaluateRules): bool, or number ≠ 0. */
export function ruleFires(v: EvalValue): boolean {
  return v === true || (typeof v === 'number' && v !== 0);
}
