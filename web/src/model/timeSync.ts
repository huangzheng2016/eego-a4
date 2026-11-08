// Set the device clock from this computer when the device has no trusted time.
// SNTP stays authoritative: a synced device is never overridden (the firmware enforces this too).
export interface DeviceTime {
  valid: boolean;
  /** Unix seconds. */
  now: number;
  sntp_synced: boolean;
}

export const TIME_DRIFT_S = 5;

export function needsTimeSync(t: DeviceTime | undefined | null, nowMs: number): boolean {
  if (!t) return false;
  if (!t.valid) return true;
  return !t.sntp_synced && Math.abs(t.now - nowMs / 1000) > TIME_DRIFT_S;
}
