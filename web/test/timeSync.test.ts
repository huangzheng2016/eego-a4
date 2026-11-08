import { describe, expect, it } from 'vitest';
import { needsTimeSync } from '../src/model/timeSync';

const now = 1_800_000_000_000;

describe('needsTimeSync', () => {
  it('syncs a device without valid time', () => {
    expect(needsTimeSync({ valid: false, now: 0, sntp_synced: false }, now)).toBe(true);
  });
  it('never overrides SNTP', () => {
    expect(needsTimeSync({ valid: true, now: now / 1000 - 3600, sntp_synced: true }, now)).toBe(false);
  });
  it('syncs an unsynced clock only past the drift threshold', () => {
    expect(needsTimeSync({ valid: true, now: now / 1000 - 3, sntp_synced: false }, now)).toBe(false);
    expect(needsTimeSync({ valid: true, now: now / 1000 - 30, sntp_synced: false }, now)).toBe(true);
  });
  it('ignores a missing status', () => {
    expect(needsTimeSync(undefined, now)).toBe(false);
  });
});
