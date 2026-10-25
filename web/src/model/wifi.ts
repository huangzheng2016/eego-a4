// WiFi network list helpers (pure; mirrors firmware net_stack/wifi_list.h).
// List order is connection priority: index 0 is tried first.

/** Placeholder the device treats as "keep the stored password". */
export const WIFI_PASSWORD_KEEP = '********';
export const WIFI_SSID_MAX_BYTES = 32;
export const WIFI_PASSWORD_MIN_BYTES = 8;
export const WIFI_PASSWORD_MAX_BYTES = 63;

export interface WifiNetworkEdit {
  ssid: string;
  /** Omitted or WIFI_PASSWORD_KEEP keeps the stored password; '' = open network. */
  password?: string;
  /** Original SSID when renaming, so the stored password can be kept. */
  previous_ssid?: string;
}

/** Editable row: `password === undefined` keeps whatever the device stores. */
export interface WifiRow {
  ssid: string;
  hasPassword: boolean;
  password?: string;
  previousSsid?: string;
}

export const utf8Bytes = (s: string) => new TextEncoder().encode(s).length;

export function rowsFromNetworks(networks: { ssid: string; has_password: boolean }[]): WifiRow[] {
  return networks.map((n) => ({ ssid: n.ssid, hasPassword: n.has_password }));
}

export function toEdits(rows: WifiRow[]): WifiNetworkEdit[] {
  return rows.map((r) => {
    const edit: WifiNetworkEdit = { ssid: r.ssid, password: r.password ?? WIFI_PASSWORD_KEEP };
    if (r.previousSsid && r.previousSsid !== r.ssid) edit.previous_ssid = r.previousSsid;
    return edit;
  });
}

/** Returns a new array with the item at `from` moved to index `to` (clamped). */
export function moveItem<T>(list: readonly T[], from: number, to: number): T[] {
  const next = list.slice();
  if (from < 0 || from >= next.length) return next;
  const target = Math.max(0, Math.min(next.length - 1, to));
  const [item] = next.splice(from, 1);
  next.splice(target, 0, item);
  return next;
}

export type WifiFieldError = 'ssid_required' | 'ssid_too_long' | 'ssid_duplicate' | 'password_required' | 'password_length';

/**
 * Validates a row about to be stored at `index` (use -1 for a new row) against
 * the other rows. A new row must state its password ('' = open network).
 */
export function validateRow(row: WifiRow, rows: WifiRow[], index: number): Partial<Record<'ssid' | 'password', WifiFieldError>> {
  const errors: Partial<Record<'ssid' | 'password', WifiFieldError>> = {};
  const len = utf8Bytes(row.ssid);
  if (!row.ssid) errors.ssid = 'ssid_required';
  else if (len > WIFI_SSID_MAX_BYTES) errors.ssid = 'ssid_too_long';
  else if (rows.some((r, i) => i !== index && r.ssid === row.ssid)) errors.ssid = 'ssid_duplicate';
  if (row.password === undefined) {
    if (index < 0) errors.password = 'password_required';
  } else if (row.password !== '') {
    const p = utf8Bytes(row.password);
    if (p < WIFI_PASSWORD_MIN_BYTES || p > WIFI_PASSWORD_MAX_BYTES) errors.password = 'password_length';
  }
  return errors;
}

export type SignalQuality = 'good' | 'fair' | 'weak';
export function signalQuality(rssi: number): SignalQuality {
  if (rssi >= -60) return 'good';
  if (rssi >= -72) return 'fair';
  return 'weak';
}

/** Common ESP-IDF wifi_err_reason_t codes (diagnostics only). */
const REASONS: Record<number, string> = {
  2: 'auth expired',
  3: 'auth leave',
  4: 'assoc expired',
  8: 'assoc leave',
  15: '4-way handshake timeout',
  200: 'beacon timeout',
  201: 'no AP found',
  202: 'auth fail',
  203: 'assoc fail',
  204: 'handshake timeout',
  205: 'connection fail',
};
export function disconnectReasonText(code: number): string {
  if (!code) return '—';
  return `${code} · ${REASONS[code] ?? 'other'}`;
}
