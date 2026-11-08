// Low-level client for the device REST API (/api/v1) over the current transport (WiFi or BLE).
// Error envelope: {"error":{"code","message","fields"?}} (admin_httpd.cpp sendError).
import { toFieldErrors, type FieldError } from '../model/errors';
import { getTransport } from './transport';

export { API_BASE } from './transport';
const TOKEN_KEY = 'eego_admin_token'; // shared with the embedded admin page

export class ApiError extends Error {
  status: number;
  code: string;
  fields: FieldError[];
  body: unknown;
  retryAfter?: number;
  constructor(status: number, code: string, message: string, fields: FieldError[] = [], body: unknown = null) {
    super(message);
    this.status = status;
    this.code = code;
    this.fields = fields;
    this.body = body;
  }
}

export function getToken(): string {
  try {
    return sessionStorage.getItem(TOKEN_KEY) ?? '';
  } catch {
    return '';
  }
}

export function setToken(token: string | null): void {
  try {
    if (token) sessionStorage.setItem(TOKEN_KEY, token);
    else sessionStorage.removeItem(TOKEN_KEY);
  } catch {
    /* storage unavailable: session lasts for this page only */
  }
  memoryToken = token ?? '';
}

let memoryToken = '';
type Listener = () => void;
const unauthorizedListeners = new Set<Listener>();
const connectivityListeners = new Set<(ok: boolean) => void>();

export function onUnauthorized(fn: Listener): () => void {
  unauthorizedListeners.add(fn);
  return () => unauthorizedListeners.delete(fn);
}

export function onConnectivity(fn: (ok: boolean) => void): () => void {
  connectivityListeners.add(fn);
  return () => connectivityListeners.delete(fn);
}

export interface RequestOptions {
  method?: 'GET' | 'POST' | 'PUT' | 'DELETE';
  body?: unknown;
  ifMatch?: number | string;
  signal?: AbortSignal;
  /** Return the raw Response instead of parsing JSON. */
  raw?: boolean;
  /** Do not broadcast 401 (used by login). */
  quiet401?: boolean;
  timeoutMs?: number;
}

// Reads are retried after a network error/timeout: on a weak WiFi link a dropped packet
// should not surface as "device unreachable". Writes are never retried here — a PUT
// whose response was lost may already have been applied.
const RETRY_DELAYS_MS = [800, 2500];

export async function request<T>(path: string, opts: RequestOptions = {}): Promise<T> {
  const read = (opts.method ?? 'GET') === 'GET';
  for (let attempt = 0; ; attempt++) {
    try {
      return await requestOnce<T>(path, opts);
    } catch (e) {
      const retry = read && e instanceof ApiError && e.status === 0 && !opts.signal?.aborted;
      if (!retry || attempt >= RETRY_DELAYS_MS.length) throw e;
      await new Promise((r) => setTimeout(r, RETRY_DELAYS_MS[attempt]));
    }
  }
}

async function requestOnce<T>(path: string, opts: RequestOptions): Promise<T> {
  const transport = getTransport();
  const headers: Record<string, string> = { Accept: 'application/json' };
  const token = getToken() || memoryToken;
  // Over BLE the bridge authorizes the paired link itself and strips Authorization.
  if (token && transport.kind === 'http') headers.Authorization = `Bearer ${token}`;
  if (opts.body !== undefined) headers['Content-Type'] = 'application/json';
  if (opts.ifMatch !== undefined) headers['If-Match'] = String(opts.ifMatch);
  const timeoutMs = (opts.timeoutMs ?? 20000) * transport.timeoutScale;
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), timeoutMs);
  opts.signal?.addEventListener('abort', () => ctrl.abort());
  let res: Response;
  try {
    res = await transport.send({
      method: opts.method ?? 'GET',
      path,
      headers,
      body: opts.body === undefined ? undefined : JSON.stringify(opts.body),
      signal: ctrl.signal,
      timeoutMs,
    });
  } catch (e) {
    connectivityListeners.forEach((fn) => fn(false));
    const aborted = (e as Error).name === 'AbortError';
    console.warn(`[eego api] ${opts.method ?? 'GET'} ${path} via ${transport.kind}: ${aborted ? `timed out after ${timeoutMs} ms` : 'unreachable'}`, e);
    throw new ApiError(0, aborted ? 'timeout' : 'network', aborted ? 'request timed out' : 'device unreachable');
  } finally {
    clearTimeout(timer);
  }
  connectivityListeners.forEach((fn) => fn(true));
  if (res.status === 401 && !opts.quiet401) unauthorizedListeners.forEach((fn) => fn());
  if (opts.raw && res.ok) return res as unknown as T;
  if (res.status === 204) return null as T;
  let body: unknown = null;
  const text = await res.text();
  if (text) {
    try {
      body = JSON.parse(text);
    } catch {
      body = text;
    }
  }
  if (!res.ok) {
    const err = (body as { error?: { code?: string; message?: string; fields?: unknown } } | null)?.error;
    const e = new ApiError(res.status, err?.code ?? `http_${res.status}`, err?.message ?? `HTTP ${res.status}`, toFieldErrors(err?.fields), body);
    console.warn(`[eego api] ${opts.method ?? 'GET'} ${path} → ${res.status} ${e.code}: ${e.message}`, body);
    const ra = res.headers.get('Retry-After');
    if (ra) e.retryAfter = Number(ra);
    throw e;
  }
  return body as T;
}
