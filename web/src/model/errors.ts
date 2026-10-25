// Map device validation field paths (e.g. "dashboard.pages[0].panels[2].grid",
// "source_plans[1].url") onto editor objects.
import type { Bundle } from './types';

export interface FieldError {
  field: string;
  message: string;
}

export type ErrorTarget =
  | { area: 'panel'; pageId: string; panelId: string; field: string }
  | { area: 'page'; pageId: string; field: string }
  | { area: 'source'; sourceId: string; field: string }
  | { area: 'variable'; name: string; field: string }
  | { area: 'alert_rule'; id: string; field: string }
  | { area: 'dashboard'; field: string };

/** Normalise the two device error shapes into a list. */
export function toFieldErrors(fields: unknown): FieldError[] {
  if (Array.isArray(fields)) {
    return fields
      .filter((e): e is { field?: unknown; message?: unknown } => !!e && typeof e === 'object')
      .map((e) => ({ field: String(e.field ?? ''), message: String(e.message ?? '') }));
  }
  if (fields && typeof fields === 'object') {
    return Object.entries(fields as Record<string, unknown>).map(([field, message]) => ({ field, message: String(message) }));
  }
  return [];
}

export function locateError(b: Bundle, field: string): ErrorTarget {
  const f = field.replace(/^dashboard\./, '');
  let m = /^pages\[(\d+)\]\.panels\[(\d+)\]\.?(.*)$/.exec(f);
  if (m) {
    const page = b.dashboard.pages[Number(m[1])];
    const panel = page?.panels[Number(m[2])];
    if (page && panel) return { area: 'panel', pageId: page.id, panelId: panel.id, field: m[3] };
  }
  m = /^pages\[(\d+)\]\.?(.*)$/.exec(f);
  if (m) {
    const page = b.dashboard.pages[Number(m[1])];
    if (page) return { area: 'page', pageId: page.id, field: m[2] };
  }
  m = /^sources\[(\d+)\]\.?(.*)$/.exec(f);
  if (m) {
    const s = b.dashboard.sources[Number(m[1])];
    if (s) return { area: 'source', sourceId: s.id, field: m[2] };
  }
  m = /^source_plans\[(\d+)\]\.?(.*)$/.exec(field);
  if (m) {
    const p = b.source_plans[Number(m[1])];
    if (p) return { area: 'source', sourceId: p.id, field: m[2] };
  }
  m = /^variables\[(\d+)\]\.?(.*)$/.exec(f);
  if (m) {
    const v = b.dashboard.variables[Number(m[1])];
    if (v) return { area: 'variable', name: v.name, field: m[2] };
  }
  m = /^alert_rules\[(\d+)\]\.?(.*)$/.exec(f);
  if (m) {
    const r = b.dashboard.alert_rules[Number(m[1])];
    if (r) return { area: 'alert_rule', id: r.id, field: m[2] };
  }
  return { area: 'dashboard', field: f };
}

/** Errors for one panel, keyed by its field ("grid", "style.variant", ...). */
export function panelErrors(b: Bundle, errors: FieldError[], panelId: string): Record<string, string> {
  const out: Record<string, string> = {};
  for (const e of errors) {
    const t = locateError(b, e.field);
    if (t.area === 'panel' && t.panelId === panelId) out[t.field || '_'] = e.message;
  }
  return out;
}

export function sourceErrors(b: Bundle, errors: FieldError[], sourceId: string): Record<string, string> {
  const out: Record<string, string> = {};
  for (const e of errors) {
    const t = locateError(b, e.field);
    if (t.area === 'source' && t.sourceId === sourceId) out[t.field || '_'] = e.message;
  }
  return out;
}
