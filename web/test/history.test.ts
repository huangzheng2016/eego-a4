import { describe, expect, it } from 'vitest';
import { commit, initHistory, redo, undo, HISTORY_LIMIT } from '../src/state/history';
import { hhmmToMinutes, minutesToHHMM } from '../src/pages/SettingsPage';

describe('undo/redo history', () => {
  it('undoes and redoes', () => {
    let h = initHistory(1);
    h = commit(h, 2);
    h = commit(h, 3);
    h = undo(h);
    expect(h.present).toBe(2);
    h = redo(h);
    expect(h.present).toBe(3);
    h = undo(undo(h));
    h = commit(h, 9);
    expect(h.future).toEqual([]);
  });
  it('coalesces rapid edits with the same key', () => {
    let h = initHistory('a');
    h = commit(h, 'ab', 'title', 1000);
    h = commit(h, 'abc', 'title', 1500);
    h = commit(h, 'abcd', 'other', 1600);
    expect(h.past).toEqual(['a', 'abc']);
  });
  it('caps history length', () => {
    let h = initHistory(0);
    for (let i = 1; i <= HISTORY_LIMIT + 20; i++) h = commit(h, i);
    expect(h.past.length).toBe(HISTORY_LIMIT);
  });
});

describe('settings time helpers', () => {
  it('converts minutes', () => {
    expect(minutesToHHMM(-1)).toBe('');
    expect(minutesToHHMM(1320)).toBe('22:00');
    expect(hhmmToMinutes('06:30')).toBe(390);
    expect(hhmmToMinutes('')).toBe(-1);
    expect(hhmmToMinutes('25:00')).toBeNull();
  });
});
