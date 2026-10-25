import { describe, expect, it } from 'vitest';
import { applyPlacement, nudgePlacement, resizeRect, resolvePlacement } from '../src/model/layout';
import { fitsScreen, layoutIssues } from '../src/model/grid';
import type { Page, Panel } from '../src/model/types';

const panel = (id: string, x: number, y: number, w: number, h: number, visible = true): Panel => ({ id, kind: 'card', grid: { x, y, w, h }, visible });
const page = (panels: Panel[], grid_columns = 24): Page => ({ id: 'p', grid_columns, panels });

describe('resolvePlacement (push aside)', () => {
  it('keeps untouched panels where they are', () => {
    const pg = page([panel('a', 0, 0, 12, 2), panel('b', 12, 0, 12, 2)]);
    const r = resolvePlacement(pg, 'a', { x: 0, y: 2, w: 12, h: 2 });
    expect(r.ok).toBe(true);
    expect(r.moved).toEqual([]);
    expect(r.grids.b).toEqual({ x: 12, y: 0, w: 12, h: 2 });
  });

  it('pushes an overlapped panel straight down within its screen', () => {
    const pg = page([panel('a', 0, 4, 24, 2), panel('b', 0, 0, 24, 2)]);
    const r = resolvePlacement(pg, 'a', { x: 0, y: 0, w: 24, h: 2 });
    expect(r.ok).toBe(true);
    expect(r.grids.a).toEqual({ x: 0, y: 0, w: 24, h: 2 });
    expect(r.grids.b).toEqual({ x: 0, y: 2, w: 24, h: 2 });
    expect(r.moved).toEqual(['b']);
  });

  it('cascades pushes in reading order', () => {
    const pg = page([panel('a', 0, 6, 24, 2), panel('b', 0, 0, 24, 2), panel('c', 0, 2, 24, 2)]);
    const r = resolvePlacement(pg, 'a', { x: 0, y: 0, w: 24, h: 3 });
    expect(r.grids.b).toEqual({ x: 0, y: 3, w: 24, h: 2 });
    expect(r.grids.c).toEqual({ x: 0, y: 5, w: 24, h: 2 });
    expect(r.ok).toBe(true);
  });

  it('fails instead of pushing a panel off the screen', () => {
    const pg = page([panel('a', 0, 0, 24, 2), panel('b', 0, 2, 24, 6)]);
    // b (h=6) cannot fit anywhere next to a (rows 2..5): a page is one screen.
    expect(resolvePlacement(pg, 'a', { x: 0, y: 2, w: 24, h: 4 }).ok).toBe(false);
    const ok = resolvePlacement(pg, 'a', { x: 0, y: 0, w: 12, h: 2 });
    expect(ok.ok).toBe(true);
    for (const g of Object.values(ok.grids)) expect(fitsScreen(g)).toBe(true);
  });

  it('moves a panel that cannot be pushed down to a free slot on the screen', () => {
    const pg = page([panel('a', 0, 4, 12, 4), panel('b', 0, 0, 12, 8)]);
    // drop a on top of b; b is 8 tall so cannot be pushed down; right half is free.
    const r = resolvePlacement(pg, 'a', { x: 0, y: 0, w: 12, h: 4 });
    expect(r.grids.b).toEqual({ x: 12, y: 0, w: 12, h: 8 });
  });

  it('avoids panels that have not been visited when searching a free slot', () => {
    const pg = page([panel('a', 0, 0, 12, 2), panel('b', 0, 2, 12, 2), panel('c', 12, 0, 12, 2)]);
    const r = resolvePlacement(pg, 'a', { x: 0, y: 2, w: 12, h: 6 });
    expect(r.ok).toBe(true);
    // b cannot be pushed below a (rows 2..7) → first free slot not used by c.
    expect(r.grids.b).toEqual({ x: 0, y: 0, w: 12, h: 2 });
    expect(r.grids.c).toEqual({ x: 12, y: 0, w: 12, h: 2 });
    const after = page(pg.panels.map((p) => ({ ...p, grid: r.grids[p.id] })));
    expect(layoutIssues(after)).toEqual([]);
  });

  it('ignores hidden panels', () => {
    const pg = page([panel('a', 0, 4, 24, 2), panel('h', 0, 0, 24, 8, false)]);
    const r = resolvePlacement(pg, 'a', { x: 0, y: 0, w: 24, h: 2 });
    expect(r.moved).toEqual([]);
    expect(r.grids.h).toBeUndefined();
  });

  it('rejects an illegal target', () => {
    const pg = page([panel('a', 0, 0, 4, 2)]);
    expect(resolvePlacement(pg, 'a', { x: 0, y: 7, w: 4, h: 2 }).ok).toBe(false);
    expect(resolvePlacement(pg, 'a', { x: 22, y: 0, w: 4, h: 2 }).ok).toBe(false);
  });

  it('supports inserting a new panel (id not on page)', () => {
    const pg = page([panel('a', 0, 0, 24, 2)]);
    const r = resolvePlacement(pg, 'new', { x: 0, y: 0, w: 12, h: 2 });
    expect(r.grids.new).toEqual({ x: 0, y: 0, w: 12, h: 2 });
    expect(r.grids.a).toEqual({ x: 0, y: 2, w: 24, h: 2 });
  });

  it('applyPlacement writes grids and the result has no overlaps', () => {
    const pg = page([panel('a', 0, 0, 8, 3), panel('b', 8, 0, 8, 3), panel('c', 16, 0, 8, 3), panel('d', 0, 3, 24, 5)]);
    const r = resolvePlacement(pg, 'd', { x: 4, y: 1, w: 16, h: 4 });
    applyPlacement(pg, r);
    expect(layoutIssues(pg)).toEqual([]);
  });
});

describe('resizeRect (edge handles)', () => {
  const o = { x: 4, y: 2, w: 8, h: 3 };
  it('east / south grow the far edges, clamped to columns and screen', () => {
    expect(resizeRect(o, 'e', 30, 0, 24)).toEqual({ x: 4, y: 2, w: 20, h: 3 });
    expect(resizeRect(o, 's', 0, 10, 24)).toEqual({ x: 4, y: 2, w: 8, h: 6 });
  });
  it('west / north move the near edges and keep the far edges', () => {
    expect(resizeRect(o, 'w', -2, 0, 24)).toEqual({ x: 2, y: 2, w: 10, h: 3 });
    expect(resizeRect(o, 'w', 20, 0, 24)).toEqual({ x: 11, y: 2, w: 1, h: 3 });
    expect(resizeRect(o, 'n', 0, -5, 24)).toEqual({ x: 4, y: 0, w: 8, h: 5 });
  });
  it('north stops at the top of the screen', () => {
    expect(resizeRect({ x: 0, y: 1, w: 4, h: 2 }, 'n', 0, -4, 24)).toEqual({ x: 0, y: 0, w: 4, h: 3 });
  });
  it('corners combine both axes', () => {
    expect(resizeRect(o, 'nw', -1, -1, 24)).toEqual({ x: 3, y: 1, w: 9, h: 4 });
    expect(resizeRect(o, 'se', 1, 1, 24)).toEqual({ x: 4, y: 2, w: 9, h: 4 });
  });
});

describe('nudgePlacement', () => {
  it('pushes the neighbour when moving into it', () => {
    const pg = page([panel('a', 0, 0, 24, 2), panel('b', 0, 2, 24, 2)]);
    const r = nudgePlacement(pg, 'a', 0, 1)!;
    expect(r.grids.a.y).toBe(1);
    expect(r.grids.b.y).toBe(3);
  });
  it('stops at the top and bottom of the screen', () => {
    expect(nudgePlacement(page([panel('a', 0, 6, 24, 2)]), 'a', 0, 1)).toBeNull();
    expect(nudgePlacement(page([panel('a', 0, 0, 24, 2)]), 'a', 0, -1)).toBeNull();
  });
  it('refuses to leave the column range', () => {
    expect(nudgePlacement(page([panel('a', 0, 0, 4, 2)]), 'a', -1, 0)).toBeNull();
  });
});
