// Low-level HTTP client for the device REST API (/api/v1, same origin).
// Error envelope: {"error":{"code","message","fields"?}} (admin_httpd.cpp sendError).
import { toFieldErrors, type FieldError } from '../model/errors';

export const API_BASE = '/api/v1';
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

export async function request<T>(path: string, opts: RequestOptions = {}): Promise<T> {
  const headers: Record<string, string> = { Accept: 'application/json' };
  const token = getToken() || memoryToken;
  if (token) headers.Authorization = `Bearer ${token}`;
  if (opts.body !== undefined) headers['Content-Type'] = 'application/json';
  if (opts.ifMatch !== undefined) headers['If-Match'] = String(opts.ifMatch);
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), opts.timeoutMs ?? 20000);
  opts.signal?.addEventListener('abort', () => ctrl.abort());
  let res: Response;
  try {
    res = await fetch(API_BASE + path, {
      method: opts.method ?? 'GET',
      headers,
      body: opts.body === undefined ? undefined : JSON.stringify(opts.body),
      signal: ctrl.signal,
      cache: 'no-store',
    });
  } catch (e) {
    connectivityListeners.forEach((fn) => fn(false));
    const aborted = (e as Error).name === 'AbortError';
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
    const ra = res.headers.get('Retry-After');
    if (ra) e.retryAfter = Number(ra);
    throw e;
  }
  return body as T;
}
