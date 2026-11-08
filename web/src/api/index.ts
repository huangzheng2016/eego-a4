// Typed endpoint catalogue. ALL device REST access goes through this module so
// endpoint changes stay in one place. Shapes follow
// firmware/components/admin/src/{admin_httpd,dashboard_api}.cpp.
import { ApiError, request, setToken, getToken } from './client';
import { PAGES_URL } from '../model/origin';
import type { Bundle, PresetDoc, SourcePlan } from '../model/types';
import type { FieldError } from '../model/errors';
import type { KeepAwakeState, PowerState } from '../model/power';
import type { WifiNetworkEdit } from '../model/wifi';

export { ApiError, getToken, setToken, onUnauthorized, onConnectivity } from './client';

/** Memoise a static read for the lifetime of the page; a failure is not cached. */
let firmwareId = '';
/** Set from /status (device.fw_sha256): keys localStorage caches of firmware-static data. */
export function setFirmwareId(id: string): void {
  firmwareId = id;
}

/**
 * Like once(), but also kept in localStorage per firmware build: data that only changes
 * with the firmware is downloaded from the device once, not on every page load.
 */
function firmwareCached<T>(name: string, fn: () => Promise<T>): () => Promise<T> {
  const load = once(async () => {
    const key = firmwareId ? `eego_fw_cache:${name}:${firmwareId}` : '';
    if (key) {
      try {
        const hit = localStorage.getItem(key);
        if (hit) return JSON.parse(hit) as T;
      } catch {
        /* storage unavailable or corrupt: fetch */
      }
    }
    const value = await fn();
    if (key) {
      try {
        for (let i = localStorage.length - 1; i >= 0; i--) {
          const k = localStorage.key(i);
          if (k?.startsWith(`eego_fw_cache:${name}:`)) localStorage.removeItem(k);
        }
        localStorage.setItem(key, JSON.stringify(value));
      } catch {
        /* quota: cache for this page only */
      }
    }
    return value;
  });
  return load;
}

function once<T>(fn: () => Promise<T>): () => Promise<T> {
  let pending: Promise<T> | null = null;
  return () => {
    pending ??= fn().catch((e: unknown) => {
      pending = null;
      throw e;
    });
    return pending;
  };
}

// ---------------------------------------------------------------- auth
export interface LoginResponse {
  token: string;
  token_type: 'Bearer';
  expires_in: number;
}
export async function login(password: string): Promise<LoginResponse> {
  const r = await request<LoginResponse>('/auth/login', { method: 'POST', body: { password }, quiet401: true });
  setToken(r.token);
  return r;
}
export async function logout(): Promise<void> {
  try {
    if (getToken()) await request('/auth/logout', { method: 'POST' });
  } finally {
    setToken(null);
  }
}
export const changePassword = (old_password: string, new_password: string) =>
  request<unknown>('/auth/change-password', { method: 'POST', body: { old_password, new_password } });

// ---------------------------------------------------------------- status
export interface DeviceStatus {
  device: { fw_version: string; fw_sha256: string; uptime_s: number; mode: string };
  network: { ssid: string; ip: string; rssi: number; wifi_state: number; ap_active: boolean; last_disconnect_reason?: number; reconnect_attempts?: number; disconnects?: number; signal?: string; signal_label?: string; signal_bars?: number; signal_hint?: string };
  time: { valid: boolean; now: number; sntp_synced: boolean; last_sync_at: number };
  memory: { internal_free: number; psram_free: number };
  storage: { sd_ready: boolean; config_valid: boolean };
  alerts: { available: boolean; critical?: number; warning?: number; info?: number; active_total?: number };
  security: { sessions: number; ingest_key_masked: string; provisioned: boolean };
  /** Keep-awake override (RAM only on the device). */
  keep_awake?: KeepAwakeState;
  screen?: DeviceScreen;
  /** Power source/sleep state published by the device UI task. */
  power?: PowerState;
  /** BLE admin link (firmware with the BLE tunnel). */
  ble?: { link: string; bonds: number };
}
/** Passive poll: does not count as activity (a battery device may still go to sleep). */
export const getStatus = () => request<DeviceStatus>('/status', { timeoutMs: 8000 });

/** Set the device clock from this computer; the device ignores it while SNTP is trusted. */
export const setDeviceTime = (unix_ms: number) =>
  request<{ applied: boolean; drift_s: number; sntp_synced: boolean }>('/device/time', {
    method: 'POST',
    body: { unix_ms, source: 'browser' },
    timeoutMs: 8000,
  });

/** First-time setup over BLE (no admin password yet; served by the BLE bridge itself). The device reboots. */
export interface SetupNetwork {
  ssid: string;
  password: string;
}
export const bleSetup = (admin_password: string, networks: SetupNetwork[] = []) =>
  request<{ status: string; rebooting: boolean }>('/setup', {
    method: 'POST',
    body: networks.length ? { admin_password, networks } : { admin_password },
    timeoutMs: 15000,
    quiet401: true,
  });

// ---------------------------------------------------------------- keep awake ("config mode")
/** minutes = 0 cancels; 1..240 keeps the device awake (no idle batching, no auto deep sleep). */
export const setKeepAwake = (minutes: number) =>
  request<KeepAwakeState>('/device/keep-awake', { method: 'POST', body: { minutes }, timeoutMs: 8000 });

/** Current dashboard screen on the device (one visible page = one screen). */
export interface DeviceScreen {
  available: boolean;
  /** 0-based */
  index: number;
  count: number;
  page_id: string;
  /** dashboard is on the display (not settings/status) */
  shown: boolean;
  rotate_min: number;
}
export const getDeviceScreen = () => request<DeviceScreen>('/device/screen');
/** Switch the displayed screen: absolute 0-based index or a wrapping step (±1). */
export const setDeviceScreen = (target: { index: number } | { step: number }) =>
  request<{ index: number; count: number }>('/device/screen', { method: 'POST', body: target, timeoutMs: 8000 });

// ---------------------------------------------------------------- system
export interface LogEntry {
  ts: number;
  unsynced: boolean;
  level: string;
  seq: number;
  msg: string;
}
export const getLogs = (limit = 200) => request<{ entries: LogEntry[] }>(`/system/logs?limit=${limit}`);
export interface CrashReport {
  reset_reason: string;
  crashed: boolean;
  task?: string;
  pc?: string;
  backtrace?: string[];
  backtrace_corrupted?: boolean;
  cause?: number;
  vaddr?: string;
  elf_sha256?: string;
}
/** Reset reason and the last crash summary (firmware coredump in flash). */
export const getCrash = () => request<CrashReport>('/system/crash', { timeoutMs: 8000 });
export const clearCrash = () => request<unknown>('/system/crash/clear', { method: 'POST', body: {} });
/** Recent ESP_LOG output kept on the device (plain text). */
export async function getDeviceLog(bytes = 16384): Promise<string> {
  const res = await request<Response>(`/system/device-log?bytes=${bytes}`, { raw: true, timeoutMs: 20000 });
  return res.text();
}
export const reboot = () => request<unknown>('/system/reboot', { method: 'POST', body: { confirm: 'REBOOT' } });
export const factoryReset = (password: string) =>
  request<unknown>('/system/factory-reset', { method: 'POST', body: { password, confirm: 'RESET' } });
export const rotateIngestKey = () => request<{ ingest_key: string }>('/system/ingest-key/rotate', { method: 'POST' });

// ---------------------------------------------------------------- alerts (device alert store)
export interface AlertRecord {
  id: string;
  type: string;
  severity: 'critical' | 'warning' | 'info';
  state: 'active' | 'acked' | 'cleared' | 'expired';
  title: string;
  message?: string;
  occurrences?: number;
  first_seen?: number;
  last_seen?: number;
  source?: { id?: string; name?: string };
  muted?: boolean;
}
export const listAlerts = (limit = 100) =>
  request<{ alerts: AlertRecord[]; total: number; snapshot_seq: number }>(`/alerts?limit=${limit}`);
export const ackAlert = (id: string) => request<unknown>(`/alerts/${encodeURIComponent(id)}/ack`, { method: 'POST' });
export const clearAlert = (id: string) => request<unknown>(`/alerts/${encodeURIComponent(id)}/clear`, { method: 'POST' });
export const ackAllAlerts = () => request<unknown>('/alerts/ack-all', { method: 'POST' });

// ---------------------------------------------------------------- settings
export interface SettingsFieldSchema {
  type: 'string' | 'int' | 'bool' | 'array' | 'number';
  /** String enums for `string` fields; numeric choices for `int` fields (e.g. idle_refresh_min). */
  enum?: (string | number)[];
  range?: [number, number];
  minimum_nonzero?: number;
  step?: number;
  default?: unknown;
  capability?: string;
  group?: string;
  note?: string;
  max_items?: number;
}
export interface SettingsSchema {
  schema_version: number;
  description?: string;
  fields: Record<string, SettingsFieldSchema>;
}
export interface SettingsResponse {
  schema_version: number;
  revision: number;
  settings: Record<string, unknown>;
  capabilities: Record<string, boolean>;
  power: Record<string, unknown>;
  ble: Record<string, unknown>;
}
export const getSettings = () => request<SettingsResponse>('/settings');
/** Built into the firmware: fetched once per page load. */
export const getSettingsSchema = firmwareCached('settings_schema', () => request<SettingsSchema>('/settings/schema'));
export const putSettings = (patch: Record<string, unknown>) =>
  request<SettingsResponse>('/settings', { method: 'PUT', body: patch });
export const bleUnbind = () => request<unknown>('/settings/ble/unbind', { method: 'POST', body: {} });

// ---------------------------------------------------------------- source credentials (write-only)
export interface CredentialMeta {
  key: string;
  ref: string; // nvs:source/<key>
}
export const listCredentials = () => request<{ namespace: string; credentials: CredentialMeta[] }>('/sources/credentials');
export const writeCredential = (key: string, value: string) =>
  request<{ key: string; ref: string; deleted: boolean }>('/sources/credentials', { method: 'PUT', body: { key, value } });
export const deleteCredential = (key: string) =>
  request<{ key: string; ref: string; deleted: boolean }>('/sources/credentials', { method: 'DELETE', body: { key } });

// ---------------------------------------------------------------- WiFi networks (passwords write-only)
export interface WifiNetwork {
  ssid: string;
  has_password: boolean;
  pass_ref: string; // nvs:wifi/<index>
}
export interface WifiCurrent {
  state: string; // disconnected | connecting | connected | failed | offline
  connected: boolean;
  ssid: string;
  rssi: number;
  ip: string;
  /** Index of the connected network in `networks`, -1 when none. */
  index: number;
  last_disconnect_reason?: number;
  reconnect_attempts?: number;
  disconnects?: number;
  driver_restarts?: number;
  offline_ms?: number;
  reconnect_backoff_ms?: number;
  /** excellent | good | fair | weak | none (firmware wifiSignalQuality). */
  signal?: string;
  signal_bars?: number;
  signal_hint?: string;
  rssi_min_10m?: number;
  rssi_avg_10m?: number;
  beacon_timeouts?: number;
  roams?: number;
  last_dhcp_ms?: number;
}
export interface WifiList {
  /** Order = connection priority. */
  networks: WifiNetwork[];
  max: number;
  current: WifiCurrent;
}
export const getWifi = () => request<WifiList>('/config/wifi');
/** Replaces the whole ordered list (add/edit/delete/reorder in one call). */
export const putWifi = (networks: WifiNetworkEdit[]) =>
  request<WifiList>('/config/wifi', { method: 'PUT', body: { networks }, timeoutMs: 15000 });

// ---------------------------------------------------------------- dashboard
export const getDashboard = () => request<Bundle>('/dashboard');
/** PUT replaces the whole bundle atomically; If-Match guards concurrent edits (409). */
export const putDashboard = (bundle: Bundle, revision: number) =>
  request<Bundle>('/dashboard', { method: 'PUT', body: bundle, ifMatch: revision });
/** POST creates when no dashboard is configured (409 otherwise). */
export const createDashboard = (bundle: Bundle) => request<Bundle>('/dashboard', { method: 'POST', body: bundle });
export interface ValidateResponse {
  valid: boolean;
  errors: FieldError[];
  bundle?: Bundle;
}
export const validateDashboard = (bundle: Bundle) =>
  request<ValidateResponse>('/dashboard/validate', { method: 'POST', body: bundle });
export const getDashboardSample = () => request<Bundle>('/dashboard/sample');
export const exportDashboard = () => request<Bundle>('/dashboard/export');

export interface PreviewMetric {
  source_id: string;
  metric: string;
  timestamp_ms: number;
  value: number;
  unit: string;
  labels: Record<string, string>;
}
export interface PreviewEvent {
  id: string;
  rule_id: string;
  source_id: string;
  timestamp_ms: number;
  severity: string;
  state: string;
  title: string;
  message: string;
  linked_panel_id: string;
}
export interface SourceTestResult {
  source_id: string;
  status_code: number;
  latency_ms: number;
  error?: string;
  metrics: PreviewMetric[];
  events: PreviewEvent[];
  assets: { kind: string; media_type: string; size_bytes: number; fetched_at_ms: number }[];
  truncated: { metrics: boolean; events: boolean };
}
/** Normalised preview (no raw body). Device rate limit: 5/min/token. */
export const testSource = (plan: SourcePlan, variables: Record<string, string>) =>
  request<SourceTestResult>('/dashboard/sources/test', { method: 'POST', body: { plan, variables }, timeoutMs: 20000 });

/** Raw response probe (bounded to 16 KiB, sensitive keys masked). Rate limit 5/min. */
export interface ProbeResult {
  source_id: string;
  status_code: number;
  latency_ms: number;
  content_type: string;
  body_bytes: number;
  error?: string;
  format: 'json' | 'text' | 'binary';
  json?: unknown;
  text?: string;
  masked_values: number;
  truncated: boolean;
}
export const probeSource = (plan: SourcePlan, variables: Record<string, string>) =>
  request<ProbeResult>('/dashboard/sources/probe', { method: 'POST', body: { plan, variables }, timeoutMs: 20000 });

/** Embedded preset library (generated from firmware/presets). */
export interface TemplateLibrary {
  schema: 'eego.dashboard-template-library/1';
  schema_version: number;
  grid_rows_per_screen: number;
  presets: PresetDoc[];
  examples: { id: string; title: string; bundle: Bundle }[];
}
/**
 * Preset library, published next to the app on GitHub Pages (scripts/postbuild.mjs runs
 * preset_tool.py library). Never fetched from the device.
 */
export const getTemplates = once(async (): Promise<TemplateLibrary> => {
  if (import.meta.env.VITE_MOCK === '1') return (await import('./mock')).templateLibrary() as TemplateLibrary;
  const url = import.meta.env.PROD ? new URL('../templates.json', import.meta.url) : new URL('templates.json', PAGES_URL);
  let res: Response;
  try {
    res = await fetch(url);
  } catch (e) {
    console.warn('[eego] templates: GitHub Pages unreachable', url.href, e);
    throw new ApiError(0, 'pages_unreachable', 'GitHub Pages unreachable');
  }
  if (!res.ok) {
    console.warn('[eego] templates: HTTP', res.status, url.href);
    throw new ApiError(res.status, 'pages_unreachable', `HTTP ${res.status}`);
  }
  return (await res.json()) as TemplateLibrary;
});

export type PreviewRefresh = 'full' | 'fast';
export type PreviewData = 'bundle' | 'live' | 'mixed' | 'placeholder' | 'none';
export interface RenderPreview {
  blob: Blob;
  page: string;
  /** 0-based screen = index of the page among visible pages (X-Preview-Screen). */
  screen: number;
  /** Visible pages = device screens (X-Preview-Screens). */
  screens: number;
  data: PreviewData | string;
}
/**
 * On-device render of a draft with the real fonts (nothing is applied).
 * 200 image/bmp 552×768 4-bpp (fast = B/W only). Errors: 400 validation_failed
 * (fields), 413, 415, 429 (10/min per session), 503 busy/timeout/unavailable.
 */
export async function renderPreview(bundle: Bundle, opts: { pageId?: string; screen?: number; refresh?: PreviewRefresh } = {}): Promise<RenderPreview> {
  // page_id and screen are alternatives (one page = one screen).
  const body: Record<string, unknown> = { bundle, refresh: opts.refresh ?? 'full' };
  if (opts.pageId) body.page_id = opts.pageId;
  else if (opts.screen !== undefined) body.screen = Math.max(0, Math.floor(opts.screen));
  const res = await request<Response>('/dashboard/render-preview', { method: 'POST', body, raw: true, timeoutMs: 30000 });
  const num = (h: string, d: number) => {
    const n = Number(res.headers.get(h));
    return Number.isFinite(n) && res.headers.get(h) !== null ? n : d;
  };
  return {
    blob: await res.blob(),
    page: res.headers.get('X-Preview-Page') ?? opts.pageId ?? '',
    screen: num('X-Preview-Screen', opts.screen ?? 0),
    screens: Math.max(1, num('X-Preview-Screens', 1)),
    data: res.headers.get('X-Preview-Data') ?? 'none',
  };
}

/** Current e-ink frame: 1-bit 552×768 BMP; 503 when unavailable. */
export async function getDisplaySnapshot(): Promise<Blob> {
  const res = await request<Response>('/display/snapshot.bmp', { raw: true, timeoutMs: 15000 });
  return res.blob();
}
