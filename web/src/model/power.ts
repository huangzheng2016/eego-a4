// Device power / keep-awake helpers (pure; unit-tested in test/power.test.ts).
// Shapes follow GET /api/v1/status `keep_awake` + `power` and
// POST /api/v1/device/keep-awake (firmware/components/admin/src/admin_httpd.cpp).

export interface KeepAwakeState {
  active: boolean;
  remaining_s: number;
  /** Unix seconds when keep-awake ends (0 when inactive or the device has no clock). */
  until: number;
  /** keep_awake_min setting (10/30/60/120). */
  default_min?: number;
}

export interface PowerState {
  source: 'mains' | 'battery' | string;
  /** forced_mains | forced_battery | no_battery | charging | usb_host | full_hold | voltage_drop | below_hold | battery */
  source_reason: string;
  power_mode: string;
  /** active | keep_awake | idle | awake | sleeping */
  mode: string;
  battery_pct: number;
  battery_mv: number;
  charging: boolean;
  usb_host: boolean;
  next_wake: number;
  next_refresh: number;
  /** Battery interactive window: seconds until automatic deep sleep (-1 = never). */
  sleep_in_s: number;
  last_wake_reason: string;
  sleeps_since_boot: number;
  last_screen_update: number;
}

export const KEEP_AWAKE_CHOICES = [10, 30, 60, 120] as const;
export const DEFAULT_KEEP_AWAKE_MIN = 30;
/** Show the "will sleep soon" banner when the device sleeps within this many seconds. */
export const SLEEP_HINT_S = 120;

/** Seconds left, counted down locally from the value fetched at `fetchedAt` (ms). */
export function keepAwakeLeft(ka: KeepAwakeState | undefined | null, fetchedAt: number, now: number): number {
  if (!ka || !ka.active) return 0;
  const elapsed = Math.max(0, Math.floor((now - fetchedAt) / 1000));
  return Math.max(0, ka.remaining_s - elapsed);
}

/** Whole minutes for display (rounded up; 0 only when inactive). */
export function minutesLeft(seconds: number): number {
  return seconds <= 0 ? 0 : Math.ceil(seconds / 60);
}

/** Seconds until battery auto sleep counted down locally, or null when it will not auto-sleep. */
export function sleepIn(power: PowerState | undefined | null, keepAwakeActive: boolean, fetchedAt: number, now: number): number | null {
  if (!power || power.source !== 'battery' || keepAwakeActive || power.sleep_in_s < 0) return null;
  const elapsed = Math.max(0, Math.floor((now - fetchedAt) / 1000));
  return Math.max(0, power.sleep_in_s - elapsed);
}

/** Battery device that will deep-sleep soon (and keep-awake is off) → show the hint banner. */
export function sleepSoon(left: number | null): boolean {
  return left !== null && left <= SLEEP_HINT_S;
}

/**
 * Should an unreachable device be presented as "probably asleep — press a key"
 * rather than a generic network error? True unless we positively know it runs
 * on mains power and was not about to sleep (a night pause also sleeps on mains).
 */
export function probablyAsleep(power: PowerState | undefined | null): boolean {
  if (!power) return true;
  return power.source === 'battery' || power.mode === 'sleeping' || power.mode === 'awake' || power.next_wake > 0;
}

/** Keep-awake toggle target: off → default duration; on → 0 (cancel). */
export function toggleMinutes(ka: KeepAwakeState | undefined | null): number {
  if (ka?.active) return 0;
  const d = ka?.default_min;
  return d && (KEEP_AWAKE_CHOICES as readonly number[]).includes(d) ? d : DEFAULT_KEEP_AWAKE_MIN;
}
