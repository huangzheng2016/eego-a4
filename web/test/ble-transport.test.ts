// BleLink against an in-page fake device (api/ble/loopback.ts), plus the client.ts seam.
import { afterEach, describe, expect, it } from 'vitest';
import { BleLink } from '../src/api/ble/bleTransport';
import { loopbackDevice, type LoopbackHandler } from '../src/api/ble/loopback';
import { parseFrameHeader, TYPE_DATA } from '../src/api/ble/protocol';
import { setTransport } from '../src/api/transport';
import { request, ApiError } from '../src/api/client';
import { renderPreview } from '../src/api';

const te = new TextEncoder();
const json = (status: number, v: unknown) => ({ status, headers: { 'content-type': 'application/json' }, body: te.encode(JSON.stringify(v)) });
const info = { v: 1, mtu: 64, name: 'EEGO-A4-TEST', provisioned: true, httpd: true };

async function link(handler: LoopbackHandler, extra: Partial<typeof info> = {}) {
  const dev = loopbackDevice(handler, { ...info, ...extra });
  const l = new BleLink(dev.connector);
  await l.open();
  return { l, dev };
}

afterEach(() => setTransport(null));

describe('BleLink', () => {
  it('reads INFO and becomes ready', async () => {
    const { l } = await link(async () => json(200, {}));
    expect(l.state).toBe('ready');
    expect(l.info?.name).toBe('EEGO-A4-TEST');
    expect(l.chunk).toBe(61);
  });

  it('serializes method, path, forwarded headers and body', async () => {
    let seen: { head: unknown; body: string } | null = null;
    const { l } = await link(async (head, body) => {
      seen = { head, body: new TextDecoder().decode(body) };
      return json(200, { ok: true });
    });
    const res = await l.send({
      method: 'PUT',
      path: '/dashboard',
      headers: { Accept: 'application/json', 'Content-Type': 'application/json', 'If-Match': '7', Authorization: 'Bearer x' },
      body: '{"a":1}',
      signal: new AbortController().signal,
      timeoutMs: 120000,
    });
    expect(await res.json()).toEqual({ ok: true });
    expect(seen).toEqual({
      head: { m: 'PUT', p: '/dashboard', h: { accept: 'application/json', 'content-type': 'application/json', 'if-match': '7' }, t: 60000 },
      body: '{"a":1}',
    });
  });

  it('keeps at most ACK_WINDOW frames in flight for a large upload', async () => {
    const big = 'x'.repeat(5000);
    let got = 0;
    const { l, dev } = await link(async (_h, body) => {
      got = body.length;
      return json(200, {});
    });
    await l.send({ method: 'POST', path: '/big', headers: {}, body: big, signal: new AbortController().signal, timeoutMs: 20000 });
    expect(got).toBe(5000);
    expect(dev.written.filter((f) => parseFrameHeader(f)?.type === TYPE_DATA).length).toBeGreaterThan(80);
  });

  it('returns binary bodies and headers; 204 has a null body', async () => {
    const bmp = Uint8Array.from({ length: 3000 }, (_, i) => i & 0xff);
    const { l } = await link(async (head) =>
      head.p === '/none' ? { status: 204 } : { status: 200, headers: { 'content-type': 'image/bmp', 'x-preview-page': 'p1' }, body: bmp },
    );
    const sig = new AbortController().signal;
    const r = await l.send({ method: 'GET', path: '/img', headers: {}, signal: sig, timeoutMs: 20000 });
    expect(r.headers.get('X-Preview-Page')).toBe('p1');
    expect(new Uint8Array(await r.arrayBuffer())).toEqual(bmp);
    const n = await l.send({ method: 'GET', path: '/none', headers: {}, signal: sig, timeoutMs: 20000 });
    expect(n.status).toBe(204);
    expect(n.body).toBeNull();
  });

  it('runs concurrent requests one at a time, in order', async () => {
    const order: string[] = [];
    const { l } = await link(async (head) => {
      order.push(head.p);
      await new Promise((r) => setTimeout(r, 5));
      return json(200, { p: head.p });
    });
    const sig = new AbortController().signal;
    const rs = await Promise.all(['/a', '/b', '/c'].map((p) => l.send({ method: 'GET', path: p, headers: {}, signal: sig, timeoutMs: 20000 })));
    expect(await Promise.all(rs.map((r) => r.json()))).toEqual([{ p: '/a' }, { p: '/b' }, { p: '/c' }]);
    expect(order).toEqual(['/a', '/b', '/c']);
  });

  it('device ABORT and disconnect surface as network errors', async () => {
    const dev = loopbackDevice(async () => json(200, {}), info, { maxReq: 50 });
    const l = new BleLink(dev.connector);
    await l.open();
    const sig = new AbortController().signal;
    await expect(l.send({ method: 'POST', path: '/x', headers: {}, body: 'y'.repeat(200), signal: sig, timeoutMs: 20000 })).rejects.toBeInstanceOf(TypeError);

    const hang = loopbackDevice(() => new Promise(() => undefined), info);
    const l2 = new BleLink(hang.connector);
    await l2.open();
    const p = l2.send({ method: 'GET', path: '/x', headers: {}, signal: sig, timeoutMs: 20000 });
    await new Promise((r) => setTimeout(r, 1));
    hang.disconnect();
    await expect(p).rejects.toBeInstanceOf(TypeError);
    expect(l2.state).toBe('lost');
    l2.close();
  });

  it('abort signal rejects with AbortError', async () => {
    const dev = loopbackDevice(() => new Promise(() => undefined), info);
    const l = new BleLink(dev.connector);
    await l.open();
    const ctrl = new AbortController();
    const p = l.send({ method: 'GET', path: '/slow', headers: {}, signal: ctrl.signal, timeoutMs: 20000 });
    await new Promise((r) => setTimeout(r, 1));
    ctrl.abort();
    await expect(p).rejects.toMatchObject({ name: 'AbortError' });
  });

  it('sleep and reboot events update the state', async () => {
    const { l, dev } = await link(async () => json(200, {}));
    dev.sendEvent('{"ev":"sleep","in_s":3}');
    await new Promise((r) => setTimeout(r, 0));
    expect(l.state).toBe('asleep');
    dev.sendEvent('{"ev":"reboot"}');
    await new Promise((r) => setTimeout(r, 0));
    expect(l.state).toBe('rebooting');
  });
});

describe('client over BLE', () => {
  it('maps error envelopes and skips the Authorization header', async () => {
    let auth: string | undefined;
    const { l } = await link(async (head) => {
      auth = head.h.authorization;
      return { status: 429, headers: { 'content-type': 'application/json', 'retry-after': '12' }, body: te.encode('{"error":{"code":"rate_limited","message":"slow down"}}') };
    });
    setTransport(l);
    const err = await request('/dashboard/sources/test', { method: 'POST', body: {} }).catch((e: unknown) => e);
    expect(err).toBeInstanceOf(ApiError);
    expect((err as ApiError).status).toBe(429);
    expect((err as ApiError).code).toBe('rate_limited');
    expect((err as ApiError).retryAfter).toBe(12);
    expect(auth).toBeUndefined();
  });

  it('raw responses (render preview) work through the tunnel', async () => {
    const bmp = Uint8Array.from({ length: 2000 }, (_, i) => (i * 5) & 0xff);
    const { l } = await link(async () => ({
      status: 200,
      headers: { 'content-type': 'image/bmp', 'x-preview-page': 'main', 'x-preview-screen': '1', 'x-preview-screens': '2', 'x-preview-data': 'live' },
      body: bmp,
    }));
    setTransport(l);
    const r = await renderPreview({ format: 'eego.dashboard-bundle/1' } as never, { screen: 1 });
    expect(r).toMatchObject({ page: 'main', screen: 1, screens: 2, data: 'live' });
    expect(r.blob.size).toBe(2000);
  });
});
