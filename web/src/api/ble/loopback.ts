// In-page fake of the device side of the BLE tunnel (used by tests and `?ble` in mock mode).
// Mirrors the firmware bridge: reassemble, ACK every ACK_WINDOW frames and at END, dispatch,
// answer with one response message, ABORT on errors.
import type { BleChannel, BleInfo, Connector } from './bleTransport';
import {
  ABORT_BUSY,
  ABORT_CRC,
  ABORT_SEQ_GAP,
  ABORT_TIMEOUT,
  ABORT_TOO_LARGE,
  encodeAbort,
  encodeAck,
  encodeEvent,
  encodeMessage,
  parseFrameHeader,
  Reassembler,
  splitMessage,
  TYPE_DATA,
  type FeedResult,
  type RequestHead,
} from './protocol';

export interface LoopbackResponse {
  status: number;
  headers?: Record<string, string>;
  body?: Uint8Array;
}
export type LoopbackHandler = (head: RequestHead, body: Uint8Array) => Promise<LoopbackResponse>;

export interface LoopbackDevice {
  connector: Connector;
  /** Frames the browser wrote, in order. */
  written: Uint8Array[];
  sendEvent(json: string): void;
  disconnect(): void;
}

const ABORT_REASON: Partial<Record<FeedResult, number>> = {
  busy: ABORT_BUSY,
  too_large: ABORT_TOO_LARGE,
  seq_gap: ABORT_SEQ_GAP,
  crc: ABORT_CRC,
  timeout: ABORT_TIMEOUT,
};

export function loopbackDevice(handler: LoopbackHandler, info: BleInfo, opts: { maxReq?: number } = {}): LoopbackDevice {
  const written: Uint8Array[] = [];
  let notify: ((f: Uint8Array) => void) | null = null;
  let lost: (() => void) | null = null;
  const chunk = info.mtu && info.mtu > 23 ? Math.min(info.mtu - 3, 512) : 20;
  const deliver = (f: Uint8Array) => queueMicrotask(() => notify?.(f));

  const channel = (): BleChannel => {
    const reasm = new Reassembler(opts.maxReq ?? info.max_req ?? 200704);
    return {
      readInfo: async () => new TextEncoder().encode(JSON.stringify(info)),
      write: async (f) => {
        written.push(f);
        const h = parseFrameHeader(f);
        if (!h || h.type !== TYPE_DATA) return;
        const res = reasm.feed(f, Date.now());
        const ack = reasm.ackDue();
        if (ack !== null && res !== 'complete') deliver(encodeAck(h.id, ack));
        if (res === 'complete') {
          const id = reasm.id();
          deliver(encodeAck(id, ack ?? 0));
          const { head, body } = splitMessage(reasm.data(), reasm.headLen());
          reasm.reset();
          // The GATT write completes immediately; the response follows as notifications.
          void handler(head as RequestHead, body).then((r) => {
            const rh = JSON.stringify({ s: r.status, h: r.headers ?? {} });
            for (const fr of encodeMessage(id, new TextEncoder().encode(rh), r.body ?? new Uint8Array(0), chunk)) deliver(fr);
          });
        } else if (res !== 'ok') {
          deliver(encodeAbort(h.id, ABORT_REASON[res] ?? ABORT_CRC));
        }
      },
      onNotify: (fn) => (notify = fn),
      onDisconnect: (fn) => (lost = fn),
      close: () => {
        notify = null;
        lost = null;
      },
    };
  };

  return {
    connector: async () => channel(),
    written,
    sendEvent: (json) => deliver(encodeEvent(json)),
    disconnect: () => {
      const l = lost;
      notify = null;
      lost = null;
      l?.();
    },
  };
}

/** Handler that forwards to `fetch` on /api/v1 (in mock mode the mock intercepts it); `token` plays the bridge's session. */
export function fetchHandler(token = '', base = '/api/v1'): LoopbackHandler {
  return async (head, body) => {
    const headers: Record<string, string> = { ...head.h };
    if (token) headers.authorization = `Bearer ${token}`;
    const res = await fetch(base + head.p, { method: head.m, headers, body: body.length ? body : undefined });
    const out: Record<string, string> = {};
    res.headers.forEach((v, k) => (out[k] = v));
    return { status: res.status, headers: out, body: new Uint8Array(await res.arrayBuffer()) };
  };
}
