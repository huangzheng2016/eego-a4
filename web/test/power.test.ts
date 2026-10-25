import { describe, expect, it } from 'vitest';
import { keepAwakeLeft, minutesLeft, probablyAsleep, sleepIn, sleepSoon, toggleMinutes, type PowerState } from '../src/model/power';
import { CONTENT, GAP_Y, ROWS_PER_SCREEN, STATUS_LINE_H, STATUS_LINE_Y } from '../src/model/grid';

const power = (over: Partial<PowerState> = {}): PowerState => ({
  source: 'battery',
  source_reason: 'battery',
  power_mode: 'auto',
  mode: 'awake',
  battery_pct: 80,
  battery_mv: 3950,
  charging: false,
  usb_host: false,
  next_wake: 0,
  next_refresh: 0,
  sleep_in_s: 45,
  last_wake_reason: 'power-key',
  sleeps_since_boot: 3,
  last_screen_update: 0,
  ...over,
});

describe('keep-awake countdown', () => {
  it('counts down locally between polls and never goes negative', () => {
    const ka = { active: true, remaining_s: 600, until: 0 };
    expect(keepAwakeLeft(ka, 1000, 1000)).toBe(600);
    expect(keepAwakeLeft(ka, 1000, 61_000)).toBe(540);
    expect(keepAwakeLeft(ka, 1000, 10_000_000)).toBe(0);
    expect(keepAwakeLeft({ active: false, remaining_s: 0, until: 0 }, 0, 0)).toBe(0);
    expect(keepAwakeLeft(undefined, 0, 0)).toBe(0);
  });
  it('rounds minutes up', () => {
    expect(minutesLeft(0)).toBe(0);
    expect(minutesLeft(1)).toBe(1);
    expect(minutesLeft(60)).toBe(1);
    expect(minutesLeft(61)).toBe(2);
  });
  it('toggles to the configured duration or cancels', () => {
    expect(toggleMinutes({ active: true, remaining_s: 5, until: 0 })).toBe(0);
    expect(toggleMinutes({ active: false, remaining_s: 0, until: 0, default_min: 60 })).toBe(60);
    expect(toggleMinutes({ active: false, remaining_s: 0, until: 0, default_min: 45 })).toBe(30);
    expect(toggleMinutes(null)).toBe(30);
  });
});

describe('sleep hint', () => {
  it('only for battery devices that will auto-sleep', () => {
    expect(sleepIn(power(), false, 0, 0)).toBe(45);
    expect(sleepIn(power(), false, 0, 20_000)).toBe(25);
    expect(sleepIn(power(), true, 0, 0)).toBeNull();
    expect(sleepIn(power({ source: 'mains' }), false, 0, 0)).toBeNull();
    expect(sleepIn(power({ sleep_in_s: -1 }), false, 0, 0)).toBeNull();
    expect(sleepSoon(45)).toBe(true);
    expect(sleepSoon(500)).toBe(false);
    expect(sleepSoon(null)).toBe(false);
  });
  it('treats an unreachable battery or unknown device as probably asleep', () => {
    expect(probablyAsleep(undefined)).toBe(true);
    expect(probablyAsleep(power())).toBe(true);
    expect(probablyAsleep(power({ source: 'mains', mode: 'active' }))).toBe(false);
    expect(probablyAsleep(power({ source: 'mains', mode: 'idle', next_wake: 1_800_000_000 }))).toBe(true);
  });
});

describe('status line geometry mirrors dashboard_runtime.cpp', () => {
  it('reserves a 40 px status line at the bottom of the UI safe area', () => {
    expect(CONTENT.y + CONTENT.h).toBe(STATUS_LINE_Y);
    expect(STATUS_LINE_Y + STATUS_LINE_H).toBe(28 + 712);
    expect((CONTENT.h + GAP_Y) % ROWS_PER_SCREEN).toBe(0);
  });
});
