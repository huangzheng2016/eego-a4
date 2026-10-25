import { describe, expect, it } from 'vitest';
import { WIFI_PASSWORD_KEEP, disconnectReasonText, moveItem, rowsFromNetworks, signalBars, signalQuality, toEdits, validateRow, type WifiRow } from '../src/model/wifi';

describe('wifi list helpers', () => {
  const rows: WifiRow[] = rowsFromNetworks([
    { ssid: 'home', has_password: true },
    { ssid: 'office', has_password: true },
    { ssid: 'cafe', has_password: false },
  ]);

  it('reorders without mutating', () => {
    expect(moveItem(rows, 2, 0).map((r) => r.ssid)).toEqual(['cafe', 'home', 'office']);
    expect(moveItem(rows, 0, 9).map((r) => r.ssid)).toEqual(['office', 'cafe', 'home']);
    expect(moveItem(rows, 5, 0)).toEqual(rows);
    expect(rows.map((r) => r.ssid)).toEqual(['home', 'office', 'cafe']);
  });

  it('keeps stored passwords unless changed, and tracks renames', () => {
    const edited: WifiRow[] = [{ ...rows[0], ssid: 'home-5g', previousSsid: 'home' }, { ...rows[1], password: 'new-password' }, { ...rows[2], password: '' }];
    expect(toEdits(edited)).toEqual([
      { ssid: 'home-5g', password: WIFI_PASSWORD_KEEP, previous_ssid: 'home' },
      { ssid: 'office', password: 'new-password' },
      { ssid: 'cafe', password: '' },
    ]);
  });

  it('validates rows like the firmware', () => {
    expect(validateRow({ ssid: '', hasPassword: false, password: '' }, rows, -1).ssid).toBe('ssid_required');
    expect(validateRow({ ssid: 'x'.repeat(33), hasPassword: false, password: '' }, rows, -1).ssid).toBe('ssid_too_long');
    expect(validateRow({ ssid: '网'.repeat(11), hasPassword: false, password: '' }, rows, -1).ssid).toBe('ssid_too_long'); // 33 bytes
    expect(validateRow({ ssid: 'office', hasPassword: true, password: '12345678' }, rows, -1).ssid).toBe('ssid_duplicate');
    expect(validateRow({ ssid: 'office', hasPassword: true }, rows, 1)).toEqual({});
    expect(validateRow({ ssid: 'new', hasPassword: true }, rows, -1).password).toBe('password_required');
    expect(validateRow({ ssid: 'new', hasPassword: true, password: 'short' }, rows, -1).password).toBe('password_length');
    expect(validateRow({ ssid: 'new', hasPassword: false, password: '' }, rows, -1)).toEqual({});
  });

  it('describes signal and disconnect reasons', () => {
    expect(signalQuality(-50)).toBe('excellent');
    expect(signalQuality(-60)).toBe('excellent');
    expect(signalQuality(-61)).toBe('good');
    expect(signalQuality(-70)).toBe('good');
    expect(signalQuality(-71)).toBe('fair');
    expect(signalQuality(-80)).toBe('fair');
    expect(signalQuality(-81)).toBe('weak');
    expect(signalQuality(-93)).toBe('weak');
    expect(signalQuality(0)).toBe('none');
    expect(signalBars('excellent')).toBe(4);
    expect(signalBars('weak')).toBe(1);
    expect(signalBars('none')).toBe(0);
    expect(disconnectReasonText(0)).toBe('—');
    expect(disconnectReasonText(201)).toBe('201 · no AP found');
    expect(disconnectReasonText(77)).toBe('77 · other');
  });
});
