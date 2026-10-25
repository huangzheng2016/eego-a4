import { describe, expect, it } from 'vitest';
import { compactNumber, displayUnit, formatNumber, formatSigned, formatTime, formatValueText, groupThousands } from '../src/model/format';

// Expected strings follow firmware panel_style.cpp (formatNumber & friends).
describe('formatNumber (panel_style.cpp port)', () => {
  it('maps units', () => {
    expect(displayUnit('percent')).toBe('%');
    expect(displayUnit('pct')).toBe('%');
    expect(displayUnit('C')).toBe('°C');
    expect(displayUnit('degF')).toBe('°F');
    expect(displayUnit('state')).toBe('');
    expect(displayUnit('req/s')).toBe('req/s');
  });
  it('groups thousands and caps decimals by magnitude', () => {
    expect(groupThousands('1234567.89')).toBe('1,234,567.89');
    expect(groupThousands('-1234')).toBe('-1,234');
    expect(formatValueText(1234.567, '', 2)).toBe('1,235');
    expect(formatValueText(123.456, '', 3)).toBe('123.5');
    expect(formatValueText(12.3456, '', 2)).toBe('12.35');
    expect(formatValueText(812345, 'tokens', 0)).toBe('812,345 tokens');
  });
  it('drops decimals for integral values and keeps two significant digits for tiny ones', () => {
    expect(formatValueText(86, 'percent', 1)).toBe('86%');
    expect(formatValueText(5, '', 2)).toBe('5');
    expect(formatValueText(0.0012, 's', 2)).toBe('0.0012 s');
    expect(formatValueText(0.004, '', 1)).toBe('0.0040'); // two significant digits, like printf
    expect(formatValueText(-0.0001, '', 0)).toBe('-0.00010');
  });
  it('symbols attach, words are spaced', () => {
    expect(formatValueText(-3.2, 'C', 1)).toBe('-3.2°C');
    expect(formatNumber(-3.2, 'C', 1)).toEqual({ number: '-3.2', unit: '°C', symbol: true });
    expect(formatValueText(25, 'req/s', 0)).toBe('25 req/s');
  });
  it('rescales bytes', () => {
    expect(formatValueText(5_300_000_000, 'B', 0)).toBe('5.3 GB');
    expect(formatValueText(42_000_000, 'bytes', 0)).toBe('42 MB');
    expect(formatValueText(512, 'B', 0)).toBe('512 B');
  });
  it('compacts only when asked (or for huge values)', () => {
    expect(formatValueText(1_912_345, 'tokens', 0)).toBe('1,912,345 tokens');
    expect(formatValueText(1_912_345, 'tokens', 0, true)).toBe('1.91M tokens');
    expect(formatValueText(9_999, '', 0, true)).toBe('9,999');
    expect(compactNumber(12_345)).toBe('12.3K');
    expect(compactNumber(999_600)).toBe('1M');
    expect(compactNumber(250_000_000)).toBe('250M');
    expect(formatNumber(2e15, '', 0).number).toBe('2.0e+15');
    expect(compactNumber(2e12)).toBe('2T');
    expect(compactNumber(5e18)).toBe('5.0e+18');
  });
  it('handles non-finite, tiny negatives and negative zero', () => {
    expect(formatValueText(NaN, '%', 1)).toBe('-- %'); // symbol flag is not set for non-finite values
    expect(formatNumber(-0.0000001, '', 0).number).toBe('-0.00000010');
    expect(formatNumber(-0.4, '', 0).number).toBe('-0.40');
    expect(formatNumber(-0.04, '', 0).number).toBe('-0.040');
    expect(formatNumber(-0.0000000001, '', 8).number).toBe('0.00000000'); // printf '-0.00000000' normalised
  });
  it('rounds exact ties to even like printf', () => {
    expect(formatValueText(-3.25, 'C', 1)).toBe('-3.2°C');
    expect(formatNumber(2.5, '', 0).number).toBe('2');
    expect(formatNumber(3.5, '', 0).number).toBe('4');
    expect(formatNumber(0.125, '', 2).number).toBe('0.12');
    expect(formatNumber(0.375, '', 2).number).toBe('0.38');
    expect(formatNumber(1.005, '', 2).number).toBe('1.00'); // 1.005 is stored as 1.00499…, not a tie

  });
  it('formatSigned adds + for positive non-zero', () => {
    expect(formatSigned(2, 'percent', 0)).toBe('+2%');
    expect(formatSigned(-1, 'percent', 0)).toBe('-1%');
    expect(formatSigned(0, '', 1)).toBe('0');
    expect(formatSigned(78126, 'tokens', 0)).toBe('+78,126 tokens');
    expect(formatSigned(4.3, '%', 1)).toBe('+4.3%');
  });
  it('formatTime renders HH:MM for epochs and counters verbatim', () => {
    expect(formatTime(42)).toBe('42');
    const d = new Date(2026, 0, 2, 7, 5);
    expect(formatTime(d.getTime())).toBe('07:05');
  });
});
