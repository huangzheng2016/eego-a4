// eego.dashboard-bundle/1 (de)serialisation, preset expansion, and template
// merging. Pure functions; no React, no network.
import {
  BUNDLE_SCHEMA,
  DASHBOARD_SCHEMA,
  PRESET_SCHEMA,
  type Bundle,
  type Dashboard,
  type Page,
  type Panel,
  type PresetDoc,
  type Source,
  type SourcePlan,
} from './types';
import { findFreeSpot, ROWS_PER_SCREEN, columnsOf, isVisible } from './grid';

export function clone<T>(v: T): T {
  return v === undefined ? v : (JSON.parse(JSON.stringify(v)) as T);
}

const DASHBOARD_ARRAYS = ['sources', 'variables', 'pages', 'alert_rules', 'assets', 'samples', 'events'] as const;

export class BundleError extends Error {}

/** Accept a bundle (or a bare eego.dashboard/2 document) and normalise it. */
export function parseBundle(raw: unknown): Bundle {
  if (!raw || typeof raw !== 'object') throw new BundleError('bundle must be an object');
  const obj = raw as Record<string, unknown>;
  if (obj.schema === PRESET_SCHEMA) return bundleFromPreset(obj as unknown as PresetDoc);
  let b: Bundle;
  if (obj.schema === BUNDLE_SCHEMA || 'dashboard' in obj) {
    b = clone(obj) as unknown as Bundle;
  } else if (obj.schema === DASHBOARD_SCHEMA || 'pages' in obj) {
    b = { schema: BUNDLE_SCHEMA, schema_version: 1, revision: 0, dashboard: clone(obj) as unknown as Dashboard, source_plans: [] };
  } else {
    throw new BundleError('unknown document schema');
  }
  if (!b.dashboard || typeof b.dashboard !== 'object') throw new BundleError('bundle.dashboard required');
  b.schema = BUNDLE_SCHEMA;
  b.schema_version = 1;
  if (typeof b.revision !== 'number') b.revision = 0;
  if (!Array.isArray(b.source_plans)) b.source_plans = [];
  const d = b.dashboard as unknown as Record<string, unknown>;
  for (const key of DASHBOARD_ARRAYS) if (!Array.isArray(d[key])) d[key] = [];
  for (const page of b.dashboard.pages) {
    if (!Array.isArray(page.panels)) page.panels = [];
    for (const panel of page.panels) if (!panel.grid) panel.grid = { x: 0, y: 0, w: 6, h: 4 };
  }
  return b;
}

/** Strip undefined values and produce the JSON body sent to the device. */
export function serializeBundle(b: Bundle): Bundle {
  return JSON.parse(JSON.stringify(b)) as Bundle;
}

export function bundleToText(b: Bundle): string {
  return JSON.stringify(serializeBundle(b), null, 2);
}

/** dashboard_validation.py bundle_from_preset(). */
export function bundleFromPreset(preset: PresetDoc, mockHost?: string): Bundle {
  if (preset.schema !== PRESET_SCHEMA) throw new BundleError('expected eego.dashboard-preset/1');
  const t = preset.dashboard;
  if (!t || !Array.isArray(t.pages) || !t.pages.length) throw new BundleError('preset dashboard must contain pages');
  const dashboard: Dashboard = {
    schema: DASHBOARD_SCHEMA,
    schema_version: 2,
    id: preset.id,
    title: preset.title ?? '',
    default_page_id: t.pages[0].id,
    refresh_ms: 60000,
    sources: clone(t.sources ?? []),
    variables: clone(t.variables ?? []),
    pages: clone(t.pages),
    alert_rules: clone(t.alert_rules ?? []),
    assets: clone(t.assets ?? []),
    samples: clone(t.samples ?? []),
    events: clone(t.events ?? []),
  };
  if (mockHost) {
    const v = dashboard.variables.find((x) => x.name === 'mock_host');
    if (v) v.default_value = mockHost;
  }
  return { schema: BUNDLE_SCHEMA, schema_version: 1, revision: 0, dashboard, source_plans: clone(preset.source_plans ?? []) };
}

export function emptyBundle(): Bundle {
  return {
    schema: BUNDLE_SCHEMA,
    schema_version: 1,
    revision: 0,
    dashboard: {
      schema: DASHBOARD_SCHEMA,
      schema_version: 2,
      id: 'dashboard',
      title: 'Dashboard',
      default_page_id: 'main',
      refresh_ms: 60000,
      sources: [],
      variables: [],
      pages: [{ id: 'main', title: 'Main', visible: true, grid_columns: 24, row_height: 16, panels: [] }],
      alert_rules: [],
      assets: [],
      samples: [],
      events: [],
    },
    source_plans: [],
  };
}

// ---------------------------------------------------------------------------
// IDs
// ---------------------------------------------------------------------------

export function uniqueId(base: string, taken: Iterable<string>): string {
  const set = new Set(taken);
  const clean = (base || 'item').replace(/[^A-Za-z0-9_-]/g, '_').slice(0, 56) || 'item';
  if (!set.has(clean)) return clean;
  for (let n = 2; ; n++) {
    const id = `${clean}-${n}`;
    if (!set.has(id)) return id;
  }
}

export function allPanelIds(d: Dashboard): string[] {
  return d.pages.flatMap((p) => p.panels.map((x) => x.id));
}

export function findPanel(d: Dashboard, panelId: string): { page: Page; panel: Panel } | null {
  for (const page of d.pages) {
    const panel = page.panels.find((p) => p.id === panelId);
    if (panel) return { page, panel };
  }
  return null;
}

/** Rename references to panels inside a set of pages. */
function renamePanelRefs(pages: Page[], map: Map<string, string>, pageMap: Map<string, string>) {
  for (const page of pages) {
    for (const p of page.panels) {
      if (map.has(p.id)) p.id = map.get(p.id)!;
      if (p.linked_panel_ids) p.linked_panel_ids = p.linked_panel_ids.map((id) => map.get(id) ?? id);
      for (const a of p.actions ?? []) {
        if (a.kind === 'goto_panel' && a.target && map.has(a.target)) a.target = map.get(a.target);
        if (a.kind === 'goto_page' && a.target && pageMap.has(a.target)) a.target = pageMap.get(a.target);
      }
    }
  }
}

function sameSource(a: Source, b: Source): boolean {
  return JSON.stringify({ ...a, id: '' }) === JSON.stringify({ ...b, id: '' });
}

/**
 * Merge the sources/plans/variables/assets of `src` into `dst`, renaming
 * colliding source IDs. Returns the source rename map.
 */
function mergeSupport(dst: Bundle, src: Bundle): Map<string, string> {
  const rename = new Map<string, string>();
  const d = dst.dashboard;
  for (const s of src.dashboard.sources) {
    const existing = d.sources.find((x) => x.id === s.id);
    const plan = src.source_plans.find((p) => p.id === s.id);
    if (existing && sameSource(existing, s)) continue;
    const id = existing ? uniqueId(s.id, d.sources.map((x) => x.id)) : s.id;
    if (id !== s.id) rename.set(s.id, id);
    d.sources.push({ ...clone(s), id });
    if (plan) dst.source_plans.push({ ...clone(plan), id } as SourcePlan);
  }
  for (const v of src.dashboard.variables) {
    if (!d.variables.some((x) => x.name === v.name)) d.variables.push(clone(v));
  }
  for (const a of src.dashboard.assets) {
    if (!d.assets.some((x) => x.id === a.id)) d.assets.push(clone(a));
  }
  return rename;
}

function prepareTemplatePages(dst: Bundle, src: Bundle, pages: Page[]): Page[] {
  const sourceRename = mergeSupport(dst, src);
  const taken = new Set(allPanelIds(dst.dashboard));
  const panelMap = new Map<string, string>();
  const pageMap = new Map<string, string>();
  const takenPages = new Set(dst.dashboard.pages.map((p) => p.id));
  const out = clone(pages);
  for (const page of out) {
    const pid = uniqueId(page.id, takenPages);
    takenPages.add(pid);
    if (pid !== page.id) pageMap.set(page.id, pid);
    for (const p of page.panels) {
      const id = uniqueId(p.id, taken);
      taken.add(id);
      if (id !== p.id) panelMap.set(p.id, id);
      if (p.source_id && sourceRename.has(p.source_id)) p.source_id = sourceRename.get(p.source_id);
    }
  }
  renamePanelRefs(out, panelMap, pageMap);
  for (const page of out) if (pageMap.has(page.id)) page.id = pageMap.get(page.id)!;
  // Alert rules that link to template panels.
  for (const rule of src.dashboard.alert_rules) {
    const r = clone(rule);
    r.id = uniqueId(r.id, dst.dashboard.alert_rules.map((x) => x.id));
    if (r.source_id && sourceRename.has(r.source_id)) r.source_id = sourceRename.get(r.source_id);
    if (r.style?.linked_panel_id && panelMap.has(r.style.linked_panel_id)) r.style.linked_panel_id = panelMap.get(r.style.linked_panel_id);
    dst.dashboard.alert_rules.push(r);
  }
  return out;
}

/** Append all pages of a template as new pages. Returns the first new page ID. */
export function addTemplateAsPages(draft: Bundle, template: Bundle): { bundle: Bundle; pageId: string } {
  const dst = clone(draft);
  const pages = prepareTemplatePages(dst, template, template.dashboard.pages);
  dst.dashboard.pages.push(...pages);
  return { bundle: dst, pageId: pages[0]?.id ?? '' };
}

/**
 * Insert a template's panels into an existing page, keeping their relative
 * arrangement and placing the block at the first free screen region.
 */
export function addTemplatePanels(draft: Bundle, pageId: string, template: Bundle): { bundle: Bundle; panelIds: string[] } {
  const dst = clone(draft);
  const page = dst.dashboard.pages.find((p) => p.id === pageId);
  if (!page) return { bundle: draft, panelIds: [] };
  const [tplPage] = prepareTemplatePages(dst, template, template.dashboard.pages.slice(0, 1));
  if (!tplPage) return { bundle: draft, panelIds: [] };
  const columns = columnsOf(page);
  const tplColumns = columnsOf(tplPage);
  const scale = columns / tplColumns;
  const visible = tplPage.panels.filter(isVisible);
  for (const p of tplPage.panels) {
    const g = p.grid;
    p.grid = {
      x: Math.min(columns - 1, Math.round(g.x * scale)),
      y: g.y,
      w: Math.max(1, Math.min(columns, Math.round(g.w * scale))),
      h: Math.min(ROWS_PER_SCREEN, g.h),
    };
  }
  // Find a block offset (whole screens) where every visible panel fits.
  const minY = visible.length ? Math.min(...visible.map((p) => p.grid.y)) : 0;
  let placed = false;
  for (let screen = 0; screen < 32 && !placed; screen++) {
    const dy = screen * ROWS_PER_SCREEN - (minY - (minY % ROWS_PER_SCREEN));
    const ok = visible.every((p) => {
      const r = { ...p.grid, y: p.grid.y + dy };
      return r.x + r.w <= columns && !page.panels.some((o) => isVisible(o) && r.x < o.grid.x + o.grid.w && o.grid.x < r.x + r.w && r.y < o.grid.y + o.grid.h && o.grid.y < r.y + r.h);
    });
    if (ok) {
      for (const p of tplPage.panels) if (isVisible(p)) p.grid.y += dy;
      placed = true;
    }
  }
  if (!placed) {
    for (const p of visible) {
      const spot = findFreeSpot(page, p.grid.w, p.grid.h);
      if (spot) p.grid = spot;
    }
  }
  page.panels.push(...tplPage.panels);
  return { bundle: dst, panelIds: tplPage.panels.map((p) => p.id) };
}

/** Replace the whole dashboard with a template, keeping the device revision. */
export function replaceWithTemplate(draft: Bundle, template: Bundle): Bundle {
  const b = clone(template);
  b.revision = draft.revision;
  return b;
}

/** Ensure a plan exists for every source (the device test endpoint needs one). */
export function planFor(b: Bundle, sourceId: string): SourcePlan | undefined {
  return b.source_plans.find((p) => p.id === sourceId);
}

export function defaultPlan(s: Source): SourcePlan {
  return {
    ...clone(s),
    headers: clone(s.headers ?? {}),
    header_secret_refs: clone(s.header_secret_refs ?? {}),
    response: 'standard_json',
    method: 'GET',
    body: '',
    static_body: '',
    text_path: '',
    reset_at_path: '$.reset_at',
    stale_after_ms: 300000,
    conditional_get: true,
    max_json_depth: 16,
  };
}

const SHARED_SOURCE_FIELDS = [
  'id',
  'kind',
  'enabled',
  'url',
  'refresh_ms',
  'timeout_ms',
  'max_response_bytes',
  'tls_verify',
  'auth_token_ref',
  'headers',
  'header_secret_refs',
] as const;

/** dashboard_api.cpp syncPlanWithSource(): the plan mirrors connection fields. */
export function syncPlan(plan: SourcePlan, s: Source): SourcePlan {
  const out = { ...plan } as Record<string, unknown>;
  const src = s as unknown as Record<string, unknown>;
  for (const k of SHARED_SOURCE_FIELDS) if (src[k] !== undefined) out[k] = clone(src[k]);
  return out as unknown as SourcePlan;
}

/** Split a plan into its dashboard source (connection fields only). */
export function sourceFromPlan(plan: SourcePlan): Source {
  const out: Record<string, unknown> = {};
  const p = plan as unknown as Record<string, unknown>;
  for (const k of SHARED_SOURCE_FIELDS) if (p[k] !== undefined) out[k] = clone(p[k]);
  return out as unknown as Source;
}
