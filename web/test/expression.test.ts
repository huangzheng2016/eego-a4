import { describe, expect, it } from 'vitest';
import { checkExpression, evaluateExpression, identifiers, ruleFires } from '../src/model/expression';

const run = (s: string, value = 85) => evaluateExpression(s, { value, now: 1000 });

describe('expression (mirror of dashboard_core/expression.cpp)', () => {
  it('evaluates comparisons and logic', () => {
    expect(run('value > 80')).toEqual({ ok: true, value: true });
    expect(run('value > 80 && value < 90')).toEqual({ ok: true, value: true });
    expect(run('value >= 90 || value == 85')).toEqual({ ok: true, value: true });
    expect(run('!(value > 80)')).toEqual({ ok: true, value: false });
  });
  it('respects precedence and conditionals', () => {
    expect(run('1 + 2 * 3')).toEqual({ ok: true, value: 7 });
    expect(run('value > 80 ? 1 : 0')).toEqual({ ok: true, value: 1 });
    expect(run('abs(-3) + max(1, 4, 2) + clamp(value, 0, 50)')).toEqual({ ok: true, value: 57 });
  });
  it('reports syntax errors with offsets', () => {
    expect(checkExpression('value >')?.message).toBe('expected operand');
    expect(checkExpression('value > 80 )')?.message).toBe('unexpected token');
    expect(checkExpression('foo(1)')?.message).toBe('unknown function');
    expect(checkExpression('')?.message).toBe('expression is empty');
    expect(checkExpression('a ? b')?.message).toBe("expected ':' in conditional expression");
    expect(checkExpression('value > 80')).toBeNull();
  });
  it('reports evaluation errors', () => {
    expect(run('cpu > 1')).toMatchObject({ ok: false, error: 'unknown identifier: cpu' });
    expect(run('value / 0')).toMatchObject({ ok: false, error: 'division by zero' });
    expect(run('value && true')).toMatchObject({ ok: false, error: 'expected a boolean' });
  });
  it('collects identifiers and maps results to rule state', () => {
    expect(identifiers('value > limit.max && now > 0').sort()).toEqual(['limit.max', 'now', 'value']);
    expect(ruleFires(true)).toBe(true);
    expect(ruleFires(2)).toBe(true);
    expect(ruleFires(0)).toBe(false);
    expect(ruleFires('x')).toBe(false);
  });
});
