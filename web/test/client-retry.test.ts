import { afterEach, describe, expect, it, vi } from 'vitest';
import { ApiError, request } from '../src/api/client';
import { setTransport, type Transport } from '../src/api/transport';

function flaky(failures: number) {
  const calls: string[] = [];
  const t: Transport = {
    kind: 'http',
    timeoutScale: 1,
    send: async (r) => {
      calls.push(r.method);
      if (calls.length <= failures) throw new TypeError('network down');
      return new Response('{"ok":true}', { status: 200 });
    },
  };
  return { t, calls };
}

afterEach(() => {
  setTransport(null);
  vi.useRealTimers();
});

describe('request retries on a flaky link', () => {
  it('retries reads twice with backoff', async () => {
    vi.useFakeTimers();
    const { t, calls } = flaky(2);
    setTransport(t);
    const p = request<{ ok: boolean }>('/status');
    await vi.runAllTimersAsync();
    await expect(p).resolves.toEqual({ ok: true });
    expect(calls).toHaveLength(3);
  });

  it('gives up after three failed reads', async () => {
    vi.useFakeTimers();
    const { t, calls } = flaky(5);
    setTransport(t);
    const p = request('/status').catch((e: unknown) => e);
    await vi.runAllTimersAsync();
    const e = await p;
    expect(e).toBeInstanceOf(ApiError);
    expect((e as ApiError).status).toBe(0);
    expect(calls).toHaveLength(3);
  });

  it('never retries writes', async () => {
    const { t, calls } = flaky(1);
    setTransport(t);
    await expect(request('/dashboard', { method: 'PUT', body: {} })).rejects.toBeInstanceOf(ApiError);
    expect(calls).toEqual(['PUT']);
  });
});
