// BLE transport: tunnels /api/v1 requests over the EEGO GATT service (protocol.ts).
// One request in flight; the device answers with one response message per request.
import type { Transport, TransportRequest } from '../transport';
import {
  ABORT_CANCELLED,
  ACK_WINDOW,
  encodeAbort,
  encodeMessage,
  INFO_UUID,
  NAME_PREFIX,
  parseFrameHeader,
  Reassembler,
  RX_UUID,
  SERVICE_UUID,
  splitMessage,
  TX_UUID,
  TYPE_ABORT,
  TYPE_ACK,
  TYPE_DATA,
  TYPE_EVENT,
  type ResponseHead,
} from './protocol';

export type BleState = 'idle' | 'connecting' | 'ready' | 'lost' | 'asleep' | 'rebooting';

export interface BleInfo {
  v: number;
  mtu?: number;
  name?: string;
  fw?: string;
  /** false: no admin password yet — only POST /setup is served. */
  provisioned?: boolean;
  httpd?: boolean;
  max_req?: number;
  max_resp?: number;
}

/** The three characteristics, abstracted so tests and the mock can supply a fake device. */
export interface BleChannel {
  readInfo(): Promise<Uint8Array>;
  write(frame: Uint8Array): Promise<void>;
  onNotify(fn: (frame: Uint8Array) => void): void;
  onDisconnect(fn: () => void): void;
  close(): void;
}
export type Connector = () => Promise<BleChannel>;

const FORWARD_HEADERS = ['content-type', 'if-match', 'accept'];
const NULL_BODY = new Set([101, 204, 205, 304]);
const DEFAULT_MAX_RESP = 393216;
const RECONNECT_DELAYS_MS = [1000, 2000, 4000, 8000, 8000, 8000, 8000, 8000, 8000];

const enc = new TextEncoder();
const networkError = (msg: string) => new TypeError(msg);
const abortError = () => Object.assign(new Error('aborted'), { name: 'AbortError' });

interface Pending {
  id: number;
  acked: number;
  wake: (() => void) | null;
  resolve: (r: Response) => void;
  reject: (e: Error) => void;
}

const ABORT_REASONS: Record<number, string> = { 1: 'busy', 2: 'too large', 3: 'sequence gap', 4: 'crc', 5: 'timeout', 6: 'cancelled' };

export class BleLink implements Transport {
  readonly kind = 'ble' as const;
  readonly timeoutScale = 2.5;
  info: BleInfo | null = null;
  state: BleState = 'idle';

  private channel: BleChannel | null = null;
  private listeners = new Set<(s: BleState) => void>();
  private queue: Promise<unknown> = Promise.resolve();
  private nextId = 0;
  private pending: Pending | null = null;
  private reasm = new Reassembler(DEFAULT_MAX_RESP);
  private closed = false;
  private reconnecting = false;

  constructor(private readonly connector: Connector) {}

  onState(fn: (s: BleState) => void): () => void {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  }

  private setState(s: BleState): void {
    if (this.state === s) return;
    log(`state ${this.state} → ${s}`);
    this.state = s;
    this.listeners.forEach((fn) => fn(s));
  }

  get chunk(): number {
    const mtu = this.info?.mtu;
    return mtu && mtu > 23 ? Math.min(mtu - 3, 512) : 20;
  }

  /** Connect (and pair, when the OS asks) and read INFO. */
  async open(): Promise<BleInfo> {
    this.closed = false;
    this.setState('connecting');
    const ch = await this.connector();
    this.channel = ch;
    ch.onNotify((f) => this.onFrame(f));
    ch.onDisconnect(() => this.onDisconnect());
    const raw = await ch.readInfo();
    this.info = JSON.parse(new TextDecoder().decode(raw)) as BleInfo;
    log('device info', this.info);
    this.reasm = new Reassembler(this.info.max_resp ?? DEFAULT_MAX_RESP);
    this.setState('ready');
    return this.info;
  }

  close(): void {
    this.closed = true;
    this.failPending(networkError('ble closed'));
    this.channel?.close();
    this.channel = null;
    this.setState('idle');
  }

  send(r: TransportRequest): Promise<Response> {
    const run = this.queue.then(() => this.sendNow(r));
    this.queue = run.catch(() => undefined);
    return run;
  }

  private async sendNow(r: TransportRequest): Promise<Response> {
    if (r.signal.aborted) throw abortError();
    const ch = this.channel;
    if (!ch || this.state !== 'ready') throw networkError('ble not connected');
    this.nextId = (this.nextId % 255) + 1;
    const id = this.nextId;
    const h: Record<string, string> = {};
    for (const [k, v] of Object.entries(r.headers)) if (FORWARD_HEADERS.includes(k.toLowerCase())) h[k.toLowerCase()] = v;
    const head = { m: r.method, p: r.path, h, t: Math.min(60000, Math.max(5000, Math.round(r.timeoutMs))) };
    const frames = encodeMessage(id, enc.encode(JSON.stringify(head)), enc.encode(r.body ?? ''), this.chunk);

    const done = new Promise<Response>((resolve, reject) => {
      this.pending = { id, acked: 0, wake: null, resolve, reject };
    });
    const p = this.pending as unknown as Pending;
    done.catch(() => undefined); // rejected while frames are still being written; awaited below
    const onAbort = () => {
      void ch.write(encodeAbort(id, ABORT_CANCELLED)).catch(() => undefined);
      this.failPending(abortError(), id);
    };
    r.signal.addEventListener('abort', onAbort);
    try {
      for (let i = 0; i < frames.length; i++) {
        while (i - p.acked >= ACK_WINDOW) {
          await new Promise<void>((wake) => (p.wake = wake));
          if (this.pending !== p) return await done;
        }
        if (this.pending !== p) break;
        await ch.write(frames[i]);
      }
      return await done;
    } catch (e) {
      this.failPending(e instanceof Error ? e : networkError(String(e)), id);
      throw e;
    } finally {
      r.signal.removeEventListener('abort', onAbort);
      if (this.pending === p) this.pending = null;
    }
  }

  private failPending(e: Error, id?: number): void {
    const p = this.pending;
    if (!p || (id !== undefined && p.id !== id)) return;
    if (e.name !== 'AbortError') warn(`request #${p.id} failed:`, e.message);
    this.pending = null;
    this.reasm.reset();
    p.wake?.();
    p.reject(e);
  }

  private onFrame(f: Uint8Array): void {
    const h = parseFrameHeader(f);
    if (!h) return;
    if (h.type === TYPE_EVENT) {
      try {
        const ev = (JSON.parse(new TextDecoder().decode(f.subarray(4))) as { ev?: string }).ev;
        if (ev === 'sleep') this.setState('asleep');
        else if (ev === 'reboot') this.setState('rebooting');
      } catch {
        /* ignore malformed events */
      }
      return;
    }
    const p = this.pending;
    if (!p || h.id !== p.id) return;
    if (h.type === TYPE_ACK) {
      if (h.seq > p.acked) p.acked = h.seq;
      const w = p.wake;
      p.wake = null;
      w?.();
    } else if (h.type === TYPE_ABORT) {
      warn(`device aborted request #${p.id}, reason ${h.seq} (${ABORT_REASONS[h.seq] ?? 'unknown'})`);
      this.failPending(networkError(`ble abort ${h.seq}`), p.id);
    } else if (h.type === TYPE_DATA) {
      const res = this.reasm.feed(f, Date.now());
      if (res === 'complete') {
        const { head, body } = splitMessage(this.reasm.data(), this.reasm.headLen());
        this.reasm.reset();
        const rh = head as ResponseHead;
        this.pending = null;
        p.wake?.();
        try {
          p.resolve(new Response(NULL_BODY.has(rh.s) ? null : body, { status: rh.s, headers: rh.h ?? {} }));
        } catch (e) {
          p.reject(e as Error);
        }
      } else if (res !== 'ok') {
        this.failPending(networkError(`ble ${res}`), p.id);
      }
    }
  }

  private onDisconnect(): void {
    this.channel = null;
    this.failPending(networkError('ble disconnected'));
    if (this.closed) return;
    if (this.state !== 'asleep' && this.state !== 'rebooting') this.setState('lost');
    void this.reconnect();
  }

  /** Retry with backoff (~60 s); no device chooser is needed for a known device. */
  async reconnect(): Promise<boolean> {
    if (this.reconnecting) return false;
    this.reconnecting = true;
    try {
      for (const [i, ms] of RECONNECT_DELAYS_MS.entries()) {
        await new Promise((r) => setTimeout(r, ms));
        if (this.closed) return false;
        log(`reconnect attempt ${i + 1}/${RECONNECT_DELAYS_MS.length}`);
        try {
          await this.open();
          return true;
        } catch (e) {
          warn('reconnect failed:', describe(e));
          this.channel?.close();
          this.channel = null;
          this.setState('lost');
        }
      }
      return false;
    } finally {
      this.reconnecting = false;
    }
  }
}

// ---------------------------------------------------------------- Web Bluetooth

export function requestBleDevice(): Promise<BluetoothDevice> {
  const bt = navigator.bluetooth;
  if (!bt) return Promise.reject(new Error('web bluetooth unavailable'));
  return bt.requestDevice({ filters: [{ services: [SERVICE_UUID] }, { namePrefix: NAME_PREFIX }], optionalServices: [SERVICE_UUID] });
}

const bytes = (v: DataView) => new Uint8Array(v.buffer.slice(v.byteOffset, v.byteOffset + v.byteLength));

const log = (...a: unknown[]) => console.info('[eego ble]', ...a);
const warn = (...a: unknown[]) => console.warn('[eego ble]', ...a);
const describe = (e: unknown) => (e instanceof Error ? `${e.name}: ${e.message}` : String(e));

/**
 * Encrypted characteristics race the link encryption on a reconnect (the device asks for
 * security right after connecting): the first access can fail while the bond is being
 * restored, so retry a few times before giving up.
 */
/** macOS/Chrome report a stale OS-level pairing (device lost its bond) this way; retrying cannot help. */
export const isStalePairing = (e: unknown) => e instanceof Error && /unsupported device/i.test(e.message);

async function withRetry<T>(what: string, fn: () => Promise<T>, attempts = 4): Promise<T> {
  for (let i = 1; ; i++) {
    try {
      return await fn();
    } catch (e) {
      warn(`${what} failed (attempt ${i}/${attempts}):`, describe(e));
      if (isStalePairing(e)) warn('the computer holds a pairing the device no longer has; remove the device in the OS Bluetooth settings and connect again');
      if (i >= attempts || isStalePairing(e)) throw e;
      await new Promise((r) => setTimeout(r, 400 * i));
    }
  }
}

export function gattConnector(device: BluetoothDevice): Connector {
  return async () => {
    const gatt = device.gatt;
    if (!gatt) throw new Error('no gatt');
    log('connecting to', device.name ?? device.id);
    const server = await withRetry('gatt.connect', () => gatt.connect(), 3);
    try {
      const svc = await withRetry('getPrimaryService', () => server.getPrimaryService(SERVICE_UUID));
      const [info, rx, tx] = await Promise.all([svc.getCharacteristic(INFO_UUID), svc.getCharacteristic(RX_UUID), svc.getCharacteristic(TX_UUID)]);
      let notify: ((f: Uint8Array) => void) | null = null;
      let lost: (() => void) | null = null;
      const onValue = (e: Event) => {
        const v = (e.target as BluetoothRemoteGATTCharacteristic).value;
        if (v) notify?.(bytes(v));
      };
      const onGone = () => {
        warn('gatt server disconnected');
        tx.removeEventListener('characteristicvaluechanged', onValue);
        device.removeEventListener('gattserverdisconnected', onGone);
        lost?.();
      };
      tx.addEventListener('characteristicvaluechanged', onValue);
      device.addEventListener('gattserverdisconnected', onGone);
      // Encrypted+authenticated characteristics: the OS pairing prompt (passkey on the device screen) appears here.
      await withRetry('startNotifications', () => tx.startNotifications());
      log('connected; notifications on');
      const write = (f: Uint8Array): Promise<void> => {
        const buf = f as unknown as BufferSource;
        if (rx.writeValueWithoutResponse) return rx.writeValueWithoutResponse(buf);
        if (rx.writeValueWithResponse) return rx.writeValueWithResponse(buf);
        return rx.writeValue ? rx.writeValue(buf) : Promise.reject(new Error('no write'));
      };
      return {
        readInfo: () => withRetry('read INFO', async () => bytes(await info.readValue())),
        write,
        onNotify: (fn) => (notify = fn),
        onDisconnect: (fn) => (lost = fn),
        close: () => {
          lost = null;
          if (server.connected) server.disconnect();
        },
      };
    } catch (e) {
      // Do not leave a half-open link behind: the device accepts one connection and
      // stops advertising while it is held, which blocks the next attempt.
      warn('connect failed, dropping link:', describe(e));
      if (server.connected) server.disconnect();
      throw e;
    }
  };
}
