// EEGO BLE tunnel, protocol v1. The firmware side (components/ble/src/tunnel_frame.cpp)
// implements the same byte format; both are checked against
// firmware/test/host/fixtures/ble_tunnel_vectors.json.
//
// Frame (little-endian):
//   byte 0   flags: bit0 START, bit1 END, bits4-7 TYPE (0 DATA, 1 ACK, 2 ABORT, 3 EVENT)
//   byte 1   message id (request id, echoed by the response; EVENT uses 0)
//   byte 2-3 seq: DATA frame index | ACK next expected seq | ABORT reason
//   byte 4.. payload
// START DATA payload begins with a 10-byte prelude: u32 total_len, u32 crc32, u16 head_len,
// where total_len and crc32 cover the message bytes = head (UTF-8 JSON) + body (raw).

export const SERVICE_UUID = 'e3e0a400-7c1b-4f7e-9d2a-5ee90a4b0000';
export const INFO_UUID = 'e3e0a400-7c1b-4f7e-9d2a-5ee90a4b0001';
export const RX_UUID = 'e3e0a400-7c1b-4f7e-9d2a-5ee90a4b0002';
export const TX_UUID = 'e3e0a400-7c1b-4f7e-9d2a-5ee90a4b0003';
export const NAME_PREFIX = 'EEGO-A4';

export const FLAG_START = 0x01;
export const FLAG_END = 0x02;
export const TYPE_DATA = 0x0;
export const TYPE_ACK = 0x1;
export const TYPE_ABORT = 0x2;
export const TYPE_EVENT = 0x3;

export const HEADER_LEN = 4;
export const PRELUDE_LEN = 10;
/** The sender waits for an ACK once this many frames are unacknowledged; the receiver ACKs every N frames and at END. */
export const ACK_WINDOW = 8;
export const IDLE_TIMEOUT_MS = 10000;

export const ABORT_BUSY = 1;
export const ABORT_TOO_LARGE = 2;
export const ABORT_SEQ_GAP = 3;
export const ABORT_CRC = 4;
export const ABORT_TIMEOUT = 5;
export const ABORT_CANCELLED = 6;

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

/** CRC-32 (IEEE 802.3 / zlib). */
export function crc32(data: Uint8Array, seed = 0): number {
  let c = (seed ^ 0xffffffff) >>> 0;
  for (let i = 0; i < data.length; i++) c = CRC_TABLE[(c ^ data[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

export interface FrameHeader {
  start: boolean;
  end: boolean;
  type: number;
  id: number;
  seq: number;
}

export function parseFrameHeader(f: Uint8Array): FrameHeader | null {
  if (f.length < HEADER_LEN) return null;
  return { start: !!(f[0] & FLAG_START), end: !!(f[0] & FLAG_END), type: f[0] >> 4, id: f[1], seq: f[2] | (f[3] << 8) };
}

function frame(flags: number, type: number, id: number, seq: number, payload: Uint8Array): Uint8Array {
  const out = new Uint8Array(HEADER_LEN + payload.length);
  out[0] = (flags & 0x03) | ((type & 0x0f) << 4);
  out[1] = id & 0xff;
  out[2] = seq & 0xff;
  out[3] = (seq >> 8) & 0xff;
  out.set(payload, HEADER_LEN);
  return out;
}

export const encodeAck = (id: number, nextSeq: number) => frame(0, TYPE_ACK, id, nextSeq, new Uint8Array(0));
export const encodeAbort = (id: number, reason: number) => frame(FLAG_START | FLAG_END, TYPE_ABORT, id, reason, new Uint8Array(0));
export const encodeEvent = (json: string) => frame(FLAG_START | FLAG_END, TYPE_EVENT, 0, 0, new TextEncoder().encode(json));

/** Split one message (head + body) into DATA frames of at most `chunk` bytes. */
export function encodeMessage(id: number, head: Uint8Array, body: Uint8Array, chunk: number): Uint8Array[] {
  if (chunk < HEADER_LEN + PRELUDE_LEN + 1) throw new Error('chunk too small');
  if (head.length > 0xffff) throw new Error('head too large');
  const msg = new Uint8Array(head.length + body.length);
  msg.set(head, 0);
  msg.set(body, head.length);
  const prelude = new Uint8Array(PRELUDE_LEN);
  const dv = new DataView(prelude.buffer);
  dv.setUint32(0, msg.length, true);
  dv.setUint32(4, crc32(msg), true);
  dv.setUint16(8, head.length, true);

  const frames: Uint8Array[] = [];
  const firstCap = chunk - HEADER_LEN - PRELUDE_LEN;
  const restCap = chunk - HEADER_LEN;
  let off = Math.min(firstCap, msg.length);
  const first = new Uint8Array(PRELUDE_LEN + off);
  first.set(prelude, 0);
  first.set(msg.subarray(0, off), PRELUDE_LEN);
  frames.push(frame(FLAG_START | (off >= msg.length ? FLAG_END : 0), TYPE_DATA, id, 0, first));
  let seq = 1;
  while (off < msg.length) {
    const n = Math.min(restCap, msg.length - off);
    const end = off + n >= msg.length;
    frames.push(frame(end ? FLAG_END : 0, TYPE_DATA, id, seq++, msg.subarray(off, off + n)));
    off += n;
  }
  return frames;
}

export type FeedResult = 'ok' | 'complete' | 'busy' | 'too_large' | 'seq_gap' | 'crc' | 'timeout' | 'bad_frame';

/** Reassembles DATA frames into one message. Same state machine as the firmware. */
export class Reassembler {
  private buf: Uint8Array | null = null;
  private filled = 0;
  private total = 0;
  private crc = 0;
  private hlen = 0;
  private nextSeq = 0;
  private msgId = -1;
  private last = 0;
  private done = false;
  private ackPending = false;

  constructor(private readonly maxSize: number) {}

  get active(): boolean {
    return this.buf !== null && !this.done;
  }
  complete(): boolean {
    return this.done;
  }
  id(): number {
    return this.msgId;
  }
  headLen(): number {
    return this.hlen;
  }
  data(): Uint8Array {
    return this.buf ? this.buf.subarray(0, this.filled) : new Uint8Array(0);
  }
  /** Next expected seq, when an ACK should be sent now. */
  ackDue(): number | null {
    if (!this.ackPending) return null;
    this.ackPending = false;
    return this.nextSeq;
  }
  reset(): void {
    this.buf = null;
    this.filled = this.total = this.crc = this.hlen = this.nextSeq = 0;
    this.msgId = -1;
    this.done = false;
    this.ackPending = false;
  }

  feed(f: Uint8Array, nowMs: number): FeedResult {
    const h = parseFrameHeader(f);
    if (!h || h.type !== TYPE_DATA) return 'bad_frame';
    if (this.buf && !this.done && nowMs - this.last > IDLE_TIMEOUT_MS) {
      this.reset();
      if (!h.start) return 'timeout';
    }
    const payload = f.subarray(HEADER_LEN);
    if (h.start) {
      if (this.buf && !this.done) return 'busy';
      if (h.seq !== 0 || payload.length < PRELUDE_LEN) return 'bad_frame';
      const dv = new DataView(payload.buffer, payload.byteOffset, PRELUDE_LEN);
      const total = dv.getUint32(0, true);
      const hlen = dv.getUint16(8, true);
      if (total > this.maxSize) return 'too_large';
      if (hlen > total) return 'bad_frame';
      this.reset();
      this.buf = new Uint8Array(total);
      this.total = total;
      this.crc = dv.getUint32(4, true);
      this.hlen = hlen;
      this.msgId = h.id;
      return this.append(payload.subarray(PRELUDE_LEN), h.end, nowMs);
    }
    if (!this.buf || this.done || h.id !== this.msgId) return 'bad_frame';
    if (h.seq !== this.nextSeq) {
      this.reset();
      return 'seq_gap';
    }
    return this.append(payload, h.end, nowMs);
  }

  private append(bytes: Uint8Array, end: boolean, nowMs: number): FeedResult {
    const buf = this.buf as Uint8Array;
    if (this.filled + bytes.length > this.total) {
      this.reset();
      return 'bad_frame';
    }
    buf.set(bytes, this.filled);
    this.filled += bytes.length;
    this.nextSeq++;
    this.last = nowMs;
    if (this.nextSeq % ACK_WINDOW === 0) this.ackPending = true;
    if (!end) return 'ok';
    if (this.filled !== this.total || crc32(buf) !== this.crc) {
      this.reset();
      return 'crc';
    }
    this.done = true;
    this.ackPending = true;
    return 'complete';
  }
}

export interface RequestHead {
  m: string;
  p: string;
  h: Record<string, string>;
  t: number;
}
export interface ResponseHead {
  s: number;
  h?: Record<string, string>;
}

export function splitMessage(msg: Uint8Array, headLen: number): { head: unknown; body: Uint8Array } {
  const head = JSON.parse(new TextDecoder().decode(msg.subarray(0, headLen))) as unknown;
  return { head, body: msg.slice(headLen) };
}

export const toHex = (b: Uint8Array) => Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
export function fromHex(s: string): Uint8Array {
  const out = new Uint8Array(s.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(s.slice(i * 2, i * 2 + 2), 16);
  return out;
}
