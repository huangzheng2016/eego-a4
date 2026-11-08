// How API requests reach the device: same-origin fetch (WiFi) or the BLE tunnel.
export const API_BASE = '/api/v1';

export interface TransportRequest {
  method: string;
  /** Path relative to /api/v1, e.g. "/status". */
  path: string;
  headers: Record<string, string>;
  body?: string;
  signal: AbortSignal;
  timeoutMs: number;
}

export interface Transport {
  kind: 'http' | 'ble';
  /** Multiplier for per-request timeouts (BLE is slower than WiFi). */
  timeoutScale: number;
  send(r: TransportRequest): Promise<Response>;
}

export const httpTransport: Transport = {
  kind: 'http',
  timeoutScale: 1,
  send: (r) =>
    fetch(API_BASE + r.path, { method: r.method, headers: r.headers, body: r.body, signal: r.signal, cache: 'no-store' }),
};

let current: Transport = httpTransport;
export const getTransport = (): Transport => current;
export function setTransport(t: Transport | null): void {
  current = t ?? httpTransport;
}
