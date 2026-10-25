// Metric catalog: which metric names a source can produce.
import type { Bundle } from './types';

export interface MetricOption {
  name: string;
  unit?: string;
  origin: 'mapping' | 'preview' | 'sample' | 'panel';
}

/** Metric names observed in sources/test previews, keyed by source ID. */
export type ObservedMetrics = Record<string, { metric: string; unit?: string }[]>;

export function metricsForSource(b: Bundle, sourceId: string, observed: ObservedMetrics = {}): MetricOption[] {
  const out = new Map<string, MetricOption>();
  const add = (name: string | undefined, origin: MetricOption['origin'], unit?: string) => {
    if (!name || out.has(name)) return;
    out.set(name, { name, unit: unit || undefined, origin });
  };
  const plan = b.source_plans.find((p) => p.id === sourceId);
  for (const m of plan?.metrics ?? []) add(m.name, 'mapping', m.target_unit || m.unit);
  for (const m of observed[sourceId] ?? []) add(m.metric, 'preview', m.unit);
  for (const s of b.dashboard.samples) if (s.source_id === sourceId) add(s.metric, 'sample', s.unit);
  for (const page of b.dashboard.pages)
    for (const p of page.panels) if (p.source_id === sourceId) add(p.metric, 'panel', p.style?.unit);
  return [...out.values()].sort((a, c) => a.name.localeCompare(c.name));
}
