import { readdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';
import { addScreen, addTemplateAsPages, addTemplatePanels, allPanelIds, bundleFromPreset, defaultPlan, deviceScreenIndex, duplicateScreen, emptyBundle, moveScreen, parseBundle, removeScreen, serializeBundle, sourceFromPlan, syncPlan } from '../src/model/bundle';
import { layoutIssues } from '../src/model/grid';
import { locateError, toFieldErrors } from '../src/model/errors';
import type { PresetDoc } from '../src/model/types';

const presets = join(__dirname, '..', '..', 'firmware', 'presets');
const load = (dir: string) => readdirSync(join(presets, dir)).filter((f) => f.endsWith('.json')).map((f) => ({ f, doc: JSON.parse(readFileSync(join(presets, dir, f), 'utf8')) }));

describe('bundle (de)serialisation', () => {
  const examples = load('examples');
  it('finds the example bundles', () => expect(examples.length).toBeGreaterThan(0));
  for (const { f, doc } of examples) {
    it(`round-trips ${f} unchanged`, () => {
      const b = parseBundle(doc);
      expect(serializeBundle(b)).toEqual(doc);
      expect(parseBundle(JSON.parse(JSON.stringify(serializeBundle(b))))).toEqual(b);
    });
    it(`${f} satisfies the screen layout rule`, () => {
      for (const page of parseBundle(doc).dashboard.pages) expect(layoutIssues(page)).toEqual([]);
    });
  }
  it('wraps a bare dashboard document', () => {
    const b = parseBundle({ schema: 'eego.dashboard/2', id: 'x', pages: [{ id: 'p', panels: [] }] });
    expect(b.schema).toBe('eego.dashboard-bundle/1');
    expect(b.dashboard.sources).toEqual([]);
    expect(b.source_plans).toEqual([]);
  });
  it('rejects unknown documents', () => expect(() => parseBundle({ foo: 1 })).toThrow());
});

describe('preset expansion (preset_tool bundle_from_preset)', () => {
  const docs = [...load('panels'), ...load('layouts')];
  for (const { f, doc } of docs) {
    it(`expands ${f}`, () => {
      const b = bundleFromPreset(doc as PresetDoc);
      expect(b).toMatchObject({ schema: 'eego.dashboard-bundle/1', schema_version: 1, revision: 0 });
      expect(b.dashboard).toMatchObject({ schema: 'eego.dashboard/2', schema_version: 2, id: doc.id, default_page_id: doc.dashboard.pages[0].id, refresh_ms: 60000 });
      for (const k of ['sources', 'variables', 'pages', 'alert_rules', 'assets', 'samples', 'events']) expect(Array.isArray((b.dashboard as unknown as Record<string, unknown>)[k])).toBe(true);
      for (const page of b.dashboard.pages) expect(layoutIssues(page)).toEqual([]);
    });
  }
  it('overrides mock_host', () => {
    const b = bundleFromPreset(docs[0].doc as PresetDoc, '10.0.0.2:8978');
    expect(b.dashboard.variables.find((v) => v.name === 'mock_host')?.default_value).toBe('10.0.0.2:8978');
  });
});

describe('template merging', () => {
  const kpi = bundleFromPreset(load('layouts').find((x) => x.f === 'chart-kpi.json')!.doc);
  it('adds template pages with unique ids and merged sources', () => {
    const base = addTemplateAsPages(emptyBundle(), kpi).bundle;
    const { bundle, pageId } = addTemplateAsPages(base, kpi);
    const ids = bundle.dashboard.pages.flatMap((p) => p.panels.map((x) => x.id));
    expect(new Set(ids).size).toBe(ids.length);
    expect(new Set(bundle.dashboard.pages.map((p) => p.id)).size).toBe(bundle.dashboard.pages.length);
    expect(pageId).not.toBe('main');
    // identical source reused, not duplicated
    expect(bundle.dashboard.sources.filter((s) => s.id.startsWith('metrics'))).toHaveLength(1);
  });
  it('renames conflicting sources and rewrites panel refs', () => {
    const base = emptyBundle();
    base.dashboard.sources.push({ id: 'metrics', kind: 'http', url: 'https://other' });
    const { bundle } = addTemplateAsPages(base, kpi);
    const newPage = bundle.dashboard.pages[1];
    expect(newPage.panels.every((p) => p.source_id === 'metrics-2')).toBe(true);
    expect(bundle.source_plans.some((p) => p.id === 'metrics-2')).toBe(true);
  });
  it('inserts panels into free space on an existing screen', () => {
    const b = emptyBundle();
    const page = b.dashboard.pages[0];
    const { bundle, panelIds } = addTemplatePanels(b, page.id, kpi);
    expect(panelIds.length).toBe(4);
    const target = bundle.dashboard.pages.find((p) => p.id === page.id)!;
    expect(target.panels.length).toBe(4 + page.panels.length);
    expect(layoutIssues(target)).toEqual([]);
  });
  it('refuses to insert into a full screen', () => {
    const b = emptyBundle();
    b.dashboard.pages[0].panels = [{ id: 'full', kind: 'text', grid: { x: 0, y: 0, w: 24, h: 8 } }];
    expect(addTemplatePanels(b, b.dashboard.pages[0].id, kpi).panelIds).toEqual([]);
  });
  it('adds templates as new screens up to 8', () => {
    let b = emptyBundle();
    for (let i = 0; i < 10; i++) b = addTemplateAsPages(b, kpi).bundle;
    expect(b.dashboard.pages).toHaveLength(8);
    const r = addTemplateAsPages(b, kpi);
    expect(r.added).toBe(0);
    expect(r.pageId).toBe('');
  });
});

describe('source/plan sync', () => {
  it('mirrors connection fields only', () => {
    const plan = { ...defaultPlan({ id: 's', kind: 'http', url: 'http://a' }), metrics: [{ name: 'x', value_path: '$.v' }] };
    const synced = syncPlan(plan, { id: 's', kind: 'http', url: 'http://b', refresh_ms: 5000 });
    expect(synced.url).toBe('http://b');
    expect(synced.metrics).toHaveLength(1);
    expect(sourceFromPlan(synced)).not.toHaveProperty('metrics');
    expect(sourceFromPlan(synced)).not.toHaveProperty('response');
  });
});

describe('validation error mapping', () => {
  const b = parseBundle(load('examples')[0].doc);
  it('maps device field paths to editor objects', () => {
    const page = b.dashboard.pages[0];
    expect(locateError(b, 'dashboard.pages[0].panels[0].grid')).toEqual({ area: 'panel', pageId: page.id, panelId: page.panels[0].id, field: 'grid' });
    expect(locateError(b, 'dashboard.sources[0].url')).toMatchObject({ area: 'source', field: 'url' });
    expect(locateError(b, 'source_plans[0].metrics')).toMatchObject({ area: 'source', field: 'metrics' });
    expect(locateError(b, 'title')).toEqual({ area: 'dashboard', field: 'title' });
  });
  it('accepts both error shapes', () => {
    expect(toFieldErrors({ a: 'x' })).toEqual([{ field: 'a', message: 'x' }]);
    expect(toFieldErrors([{ field: 'b', message: 'y' }])).toEqual([{ field: 'b', message: 'y' }]);
  });
});

describe('screens (one page = one screen)', () => {
  it('adds up to 8 screens', () => {
    const b = emptyBundle();
    for (let i = 0; i < 10; i++) addScreen(b.dashboard, `S${i}`);
    expect(b.dashboard.pages).toHaveLength(8);
    expect(addScreen(b.dashboard, 'x')).toBe('');
  });
  it('duplicates a screen with fresh ids and internal references', () => {
    const b = emptyBundle();
    b.dashboard.pages[0].panels = [
      { id: 'a', kind: 'card', grid: { x: 0, y: 0, w: 12, h: 2 }, linked_panel_ids: ['b'] },
      { id: 'b', kind: 'button', grid: { x: 12, y: 0, w: 12, h: 2 }, actions: [{ id: 'go', kind: 'goto_panel', label: 'A', target: 'a' }] },
    ];
    const id = duplicateScreen(b.dashboard, 'main', 'Copy');
    expect(b.dashboard.pages.map((p) => p.id)).toEqual(['main', id]);
    const copy = b.dashboard.pages[1];
    expect(copy.title).toBe('Copy');
    const ids = allPanelIds(b.dashboard);
    expect(new Set(ids).size).toBe(4);
    expect(copy.panels[0].linked_panel_ids).toEqual([copy.panels[1].id]);
    expect(copy.panels[1].actions?.[0].target).toBe(copy.panels[0].id);
  });
  it('removes and reorders screens, keeping the default page valid', () => {
    const b = emptyBundle();
    const two = addScreen(b.dashboard, 'Two');
    const three = addScreen(b.dashboard, 'Three');
    moveScreen(b.dashboard, 2, 0);
    expect(b.dashboard.pages.map((p) => p.id)).toEqual([three, 'main', two]);
    expect(removeScreen(b.dashboard, 'main')).toBe(true);
    expect(b.dashboard.default_page_id).toBe(three);
    b.dashboard.pages[1].visible = false;
    expect(deviceScreenIndex(b.dashboard, two)).toBe(-1);
    expect(deviceScreenIndex(b.dashboard, three)).toBe(0);
    removeScreen(b.dashboard, two);
    expect(removeScreen(b.dashboard, three)).toBe(false); // never the last screen
  });
});
