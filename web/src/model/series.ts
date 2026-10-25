// Data used to draw panels in the editor: real samples from the bundle
// (dashboard.samples, like panels.cpp series()) plus values seen in the last
// sources/test preview; otherwise a deterministic demo series so the canvas
// still shows what a panel will look like.
import type { Bundle, Panel } from './types';
import type { ObservedMetrics } from './metrics';

export interface Point {
  t: number;
  v: number;
  labels?: Record<string, string>;
}
export interface Series {
  points: Point[];
  unit: string;
  demo: boolean;
}

function hash(s: string): number {
  let h = 2166136261;
  for (let i = 0; i < s.length; i++) h = Math.imul(h ^ s.charCodeAt(i), 16777619);
  return h >>> 0;
}

/** Deterministic pseudo-random walk seeded by `seed`. */
export function demoSeries(seed: string, n = 24, lo = 20, hi = 80): number[] {
  let x = hash(seed) || 1;
  const rnd = () => ((x = Math.imul(x ^ (x >>> 15), 2246822519) ^ Math.imul(x ^ (x >>> 13), 3266489917)) >>> 0) / 4294967296;
  let v = lo + (hi - lo) * (0.35 + rnd() * 0.3);
  const out: number[] = [];
  for (let i = 0; i < n; i++) {
    v += (rnd() - 0.48) * (hi - lo) * 0.18;
    v = Math.max(lo, Math.min(hi, v));
    out.push(Math.round(v * 10) / 10);
  }
  return out;
}

export function panelSeries(b: Bundle | null, panel: Panel, observed: ObservedMetrics = {}): Series {
  const points: Point[] = [];
  let unit = panel.style?.unit ?? '';
  if (b && panel.metric) {
    for (const s of b.dashboard.samples) {
      if (s.source_id === (panel.source_id ?? '') && s.metric === panel.metric && Number.isFinite(s.value)) {
        points.push({ t: s.timestamp_ms ?? 0, v: s.value, labels: s.labels });
        if (!unit && s.unit) unit = s.unit;
      }
    }
    points.sort((a, c) => a.t - c.t);
    for (const m of observed[panel.source_id ?? ''] ?? []) {
      if (m.metric === panel.metric && typeof m.value === 'number') {
        points.push({ t: m.timestamp_ms ?? Date.now(), v: m.value, labels: m.labels });
        if (!unit && m.unit) unit = m.unit;
      }
    }
  }
  if (points.length) return { points, unit, demo: false };
  // Like the device, a panel without data shows "No data" once the dashboard
  // has real data somewhere; demo values only fill fully data-less drafts.
  if (b && (b.dashboard.samples.length > 0 || Object.values(observed).some((l) => l.length))) return { points: [], unit, demo: false };
  const progress = panel.style?.variant === 'progress' || unit === '%' || unit === 'percent';
  const vals = demoSeries(panel.id + (panel.metric ?? ''), 24, progress ? 30 : 10, progress ? 90 : 99);
  // Epoch timestamps 5 min apart (charts show HH:MM like on the device).
  const end = Math.floor(Date.now() / 300000) * 300000;
  const names = ['api', 'db', 'cache', 'queue', 'cdn', 'auth', 'mail', 'dns'];
  return { points: vals.map((v, i) => ({ t: end - (vals.length - 1 - i) * 300000, v, labels: { name: names[i % names.length] } })), unit, demo: true };
}

/** Latest value of a metric (for rule previews). */
export function latestValue(b: Bundle, observed: ObservedMetrics, sourceId: string, metric: string): { value: number; unit?: string; live: boolean } | null {
  const obs = (observed[sourceId] ?? []).filter((m) => m.metric === metric && typeof m.value === 'number');
  if (obs.length) return { value: obs[obs.length - 1].value as number, unit: obs[obs.length - 1].unit, live: true };
  let best: { value: number; unit?: string; t: number } | null = null;
  for (const s of b.dashboard.samples)
    if (s.source_id === sourceId && s.metric === metric && (!best || (s.timestamp_ms ?? 0) >= best.t)) best = { value: s.value, unit: s.unit, t: s.timestamp_ms ?? 0 };
  return best ? { value: best.value, unit: best.unit, live: false } : null;
}

/** panels.cpp formatValue(). */
export function formatValue(v: number, unit: string, precision = 1): string {
  if (!Number.isFinite(v)) return '--';
  const p = Math.min(8, Math.max(0, precision));
  const mag = Math.abs(v);
  const n = (mag !== 0 && mag < 0.0001) || mag >= 1e9 ? v.toPrecision(p + 1) : v.toFixed(p);
  return unit ? (unit === '%' ? n + unit : `${n} ${unit}`) : n;
}
