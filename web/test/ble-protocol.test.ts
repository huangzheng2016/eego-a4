// BLE tunnel byte format. The vectors file is shared with the firmware host tests
// (firmware/test/host/test_ble_tunnel.cpp); regenerate with UPDATE_BLE_VECTORS=1 npm test.
import { describe, expect, it } from 'vitest';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import {
  ACK_WINDOW,
  crc32,
  encodeAbort,
  encodeAck,
  encodeEvent,
  encodeMessage,
  fromHex,
  IDLE_TIMEOUT_MS,
  Reassembler,
  splitMessage,
  toHex,
} from '../src/api/ble/protocol';

const FIXTURE = resolve(__dirname, '../../firmware/test/host/fixtures/ble_tunnel_vectors.json');
const te = new TextEncoder();

interface MessageVector {
  name: string;
  id: number;
  chunk: number;
  head: string;
  body_hex: string;
  frames: string[];
}
interface Vectors {
  version: number;
  crc32: { input_hex: string; crc: string }[];
  messages: MessageVector[];
  control: { name: string; frame: string }[];
}

const pattern = (n: number, mul: number) => Uint8Array.from({ length: n }, (_, i) => (i * mul) & 0xff);

function build(): Vectors {
  const crcInputs = [te.encode('123456789'), new Uint8Array(0), te.encode('a'), pattern(256, 1)];
  const msgs: Omit<MessageVector, 'frames'>[] = [
    { name: 'get_status_chunk20', id: 1, chunk: 20, head: '{"m":"GET","p":"/status","h":{"accept":"application/json"},"t":20000}', body_hex: '' },
    {
      name: 'put_dashboard_chunk244',
      id: 2,
      chunk: 244,
      head: '{"m":"PUT","p":"/dashboard","h":{"content-type":"application/json","if-match":"3"},"t":50000}',
      body_hex: toHex(te.encode(JSON.stringify({ format: 'eego.dashboard/2', schema_version: 2, pages: [{ id: 'p1', title: '主屏', panels: Array.from({ length: 6 }, (_, i) => ({ id: `n${i}`, kind: 'card' })) }] }))),
    },
    { name: 'bmp_response_chunk509', id: 255, chunk: 509, head: '{"s":200,"h":{"content-type":"image/bmp"}}', body_hex: toHex(pattern(1200, 7)) },
    { name: 'no_content_single_frame', id: 3, chunk: 509, head: '{"s":204}', body_hex: '' },
  ];
  return {
    version: 1,
    crc32: crcInputs.map((b) => ({ input_hex: toHex(b), crc: crc32(b).toString(16).padStart(8, '0') })),
    messages: msgs.map((m) => ({ ...m, frames: encodeMessage(m.id, te.encode(m.head), fromHex(m.body_hex), m.chunk).map(toHex) })),
    control: [
      { name: 'ack_id2_seq8', frame: toHex(encodeAck(2, 8)) },
      { name: 'abort_id5_crc', frame: toHex(encodeAbort(5, 4)) },
      { name: 'event_sleep', frame: toHex(encodeEvent('{"ev":"sleep","in_s":3}')) },
    ],
  };
}

if (process.env.UPDATE_BLE_VECTORS || !existsSync(FIXTURE)) {
  mkdirSync(dirname(FIXTURE), { recursive: true });
  writeFileSync(FIXTURE, JSON.stringify(build(), null, 2) + '\n');
}
const vectors = JSON.parse(readFileSync(FIXTURE, 'utf8')) as Vectors;

describe('crc32', () => {
  it('matches the standard check value', () => {
    expect(crc32(te.encode('123456789'))).toBe(0xcbf43926);
  });
  it.each(vectors.crc32)('fixture %#', (v) => {
    expect(crc32(fromHex(v.input_hex)).toString(16).padStart(8, '0')).toBe(v.crc);
  });
});

describe('fixture encodings', () => {
  it.each(vectors.messages)('$name encodes byte-for-byte', (m) => {
    expect(encodeMessage(m.id, te.encode(m.head), fromHex(m.body_hex), m.chunk).map(toHex)).toEqual(m.frames);
  });
  it.each(vectors.messages)('$name reassembles', (m) => {
    const r = new Reassembler(1 << 20);
    const results = m.frames.map((f) => r.feed(fromHex(f), 0));
    expect(results.at(-1)).toBe('complete');
    expect(results.slice(0, -1).every((x) => x === 'ok')).toBe(true);
    expect(r.id()).toBe(m.id);
    const { head, body } = splitMessage(r.data(), r.headLen());
    expect(head).toEqual(JSON.parse(m.head));
    expect(toHex(body)).toBe(m.body_hex);
  });
  it('control frames', () => {
    const byName = Object.fromEntries(vectors.control.map((c) => [c.name, c.frame]));
    expect(toHex(encodeAck(2, 8))).toBe(byName.ack_id2_seq8);
    expect(toHex(encodeAbort(5, 4))).toBe(byName.abort_id5_crc);
    expect(toHex(encodeEvent('{"ev":"sleep","in_s":3}'))).toBe(byName.event_sleep);
  });
});

describe('Reassembler', () => {
  const head = te.encode('{"m":"GET","p":"/x"}');
  const frames = encodeMessage(9, head, pattern(400, 3), 20);

  it('frame layout: START prelude, then raw continuation', () => {
    expect(frames[0][0]).toBe(0x01);
    expect(frames.at(-1)?.[0]).toBe(0x02);
    expect(frames[1][2] | (frames[1][3] << 8)).toBe(1);
    const single = encodeMessage(1, te.encode('{}'), new Uint8Array(0), 509);
    expect(single).toHaveLength(1);
    expect(single[0][0]).toBe(0x03);
  });
  it('acks every window and at END', () => {
    const r = new Reassembler(1 << 20);
    const acks: number[] = [];
    for (const f of frames) {
      r.feed(f, 0);
      const a = r.ackDue();
      if (a !== null) acks.push(a);
    }
    const n = frames.length;
    const expected = Array.from({ length: Math.floor(n / ACK_WINDOW) }, (_, i) => (i + 1) * ACK_WINDOW);
    if (n % ACK_WINDOW !== 0) expected.push(n);
    expect(acks).toEqual(expected);
  });
  it('rejects a seq gap', () => {
    const r = new Reassembler(1 << 20);
    r.feed(frames[0], 0);
    expect(r.feed(frames[2], 0)).toBe('seq_gap');
  });
  it('rejects a CRC mismatch', () => {
    const r = new Reassembler(1 << 20);
    const bad = frames.map((f) => f.slice());
    bad[1][6] ^= 0xff;
    const res = bad.map((f) => r.feed(f, 0));
    expect(res.at(-1)).toBe('crc');
  });
  it('rejects oversize messages and START while busy', () => {
    expect(new Reassembler(100).feed(frames[0], 0)).toBe('too_large');
    const r = new Reassembler(1 << 20);
    r.feed(frames[0], 0);
    expect(r.feed(frames[0], 0)).toBe('busy');
  });
  it('drops a stalled message after the idle timeout', () => {
    const r = new Reassembler(1 << 20);
    r.feed(frames[0], 0);
    expect(r.feed(frames[1], IDLE_TIMEOUT_MS + 1)).toBe('timeout');
    expect(r.feed(frames[0], IDLE_TIMEOUT_MS + 2)).toBe('ok');
  });
  it('accepts an empty body', () => {
    const r = new Reassembler(1 << 20);
    const fr = encodeMessage(4, te.encode('{"s":200}'), new Uint8Array(0), 20);
    const res = fr.map((f) => r.feed(f, 0));
    expect(res.at(-1)).toBe('complete');
    expect(splitMessage(r.data(), r.headLen()).body).toHaveLength(0);
  });
});
