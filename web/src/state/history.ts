// Undo/redo history for the dashboard draft. Pure; snapshots are immutable
// (callers never mutate `present`, they produce a new object).
export interface History<T> {
  past: T[];
  present: T;
  future: T[];
  /** Consecutive commits with the same key merge into one undo step. */
  lastKey?: string;
  lastAt?: number;
}

export const HISTORY_LIMIT = 100;
const COALESCE_MS = 1200;

export function initHistory<T>(present: T): History<T> {
  return { past: [], present, future: [] };
}

export function commit<T>(h: History<T>, next: T, key?: string, now = Date.now()): History<T> {
  if (next === h.present) return h;
  if (key && key === h.lastKey && h.lastAt !== undefined && now - h.lastAt < COALESCE_MS) {
    return { ...h, present: next, future: [], lastAt: now };
  }
  const past = [...h.past, h.present];
  if (past.length > HISTORY_LIMIT) past.shift();
  return { past, present: next, future: [], lastKey: key, lastAt: now };
}

export function undo<T>(h: History<T>): History<T> {
  if (!h.past.length) return h;
  const past = h.past.slice(0, -1);
  return { past, present: h.past[h.past.length - 1], future: [h.present, ...h.future] };
}

export function redo<T>(h: History<T>): History<T> {
  if (!h.future.length) return h;
  const [present, ...future] = h.future;
  return { past: [...h.past, h.present], present, future };
}

/** Replace without an undo step (e.g. loading from the device). */
export function reset<T>(present: T): History<T> {
  return initHistory(present);
}
