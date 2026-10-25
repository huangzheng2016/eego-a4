import { describe, expect, it } from 'vitest';
import { canPlace, clampRect, clampResize, edge, findFreeSpot, fitsScreen, layoutIssues, nudge, panelPixels, screenCount, screenIndex, snapDelta, CONTENT } from '../src/model/grid';
import type { Page, Panel } from '../src/model/types';

const panel = (id: string, x: number, y: number, w: number, h: number, visible = true): Panel => ({ id, kind: 'card', grid: { x, y, w, h }, visible });
const page = (panels: Panel[], grid_columns = 24): Page => ({ id: 'p', grid_columns, panels });

describe('screen rule (model.h kGridRowsPerScreen = 8)', () => {
  it('fitsScreen mirrors gridFitsScreen', () => {
    expect(fitsScreen({ x: 0, y: 0, w: 1, h: 8 })).toBe(true);
    expect(fitsScreen({ x: 0, y: 6, w: 1, h: 3 })).toBe(false);
    expect(fitsScreen({ x: 0, y: 8, w: 1, h: 8 })).toBe(true);
    expect(fitsScreen({ x: 0, y: 0, w: 1, h: 9 })).toBe(false);
  });
  it('screenIndex moves crossing panels to the next screen', () => {
    expect(screenIndex({ x: 0, y: 7, w: 1, h: 1 })).toBe(0);
    expect(screenIndex({ x: 0, y: 7, w: 1, h: 2 })).toBe(1);
    expect(screenIndex({ x: 0, y: 16, w: 1, h: 2 })).toBe(2);
  });
  it('screenCount counts visible panels only', () => {
    expect(screenCount(page([panel('a', 0, 0, 4, 4), panel('b', 0, 20, 4, 4, false)]))).toBe(1);
    expect(screenCount(page([panel('a', 0, 9, 4, 4)]))).toBe(2);
  });
});

describe('pixel mapping (layout.cpp edge)', () => {
  it('maps the full grid onto the content rect', () => {
    const px = panelPixels({ x: 0, y: 0, w: 24, h: 8 }, 24);
    expect(px).toMatchObject({ x: CONTENT.x, y: CONTENT.y, w: CONTENT.w, h: CONTENT.h, screen: 0 });
  });
  it('separates adjacent panels by exactly one gutter', () => {
    const a = panelPixels({ x: 0, y: 0, w: 12, h: 4 }, 24);
    const b = panelPixels({ x: 12, y: 0, w: 12, h: 4 }, 24);
    expect(b.x - (a.x + a.w)).toBe(8);
    expect(edge(12, 24, 496, 8)).toBe(252);
  });
});

describe('snapping and clamping', () => {
  it('snaps pixel deltas to whole cells', () => {
    expect(snapDelta(21, 0, 24)).toEqual({ dx: 1, dy: 0 });
    expect(snapDelta(-40, 100, 24)).toEqual({ dx: -2, dy: 1 });
  });
  it('clampRect keeps the rect inside columns and one screen', () => {
    expect(clampRect({ x: 22, y: 0, w: 6, h: 2 }, 24)).toEqual({ x: 18, y: 0, w: 6, h: 2 });
    expect(clampRect({ x: 0, y: 6, w: 4, h: 4 }, 24)).toEqual({ x: 0, y: 4, w: 4, h: 4 });
    expect(clampRect({ x: 0, y: 6, w: 4, h: 4 }, 24, true)).toEqual({ x: 0, y: 8, w: 4, h: 4 });
    expect(clampRect({ x: -3, y: -1, w: 40, h: 20 }, 24)).toEqual({ x: 0, y: 0, w: 24, h: 8 });
  });
  it('clampResize caps size at the screen bottom', () => {
    expect(clampResize({ x: 20, y: 5, w: 10, h: 10 }, 24)).toEqual({ x: 20, y: 5, w: 4, h: 3 });
  });
});

describe('overlap and placement', () => {
  const pg = page([panel('a', 0, 0, 12, 4), panel('b', 12, 0, 12, 4), panel('hidden', 0, 4, 24, 4, false)]);
  it('refuses overlapping or screen-crossing placements', () => {
    expect(canPlace(pg, { x: 6, y: 0, w: 4, h: 2 }, 'x')).toBe(false);
    expect(canPlace(pg, { x: 0, y: 4, w: 24, h: 4 }, 'x')).toBe(true); // hidden panels do not block
    expect(canPlace(pg, { x: 0, y: 6, w: 4, h: 4 }, 'x')).toBe(false);
    expect(canPlace(pg, { x: 0, y: 0, w: 12, h: 4 }, 'a')).toBe(true); // itself
  });
  it('findFreeSpot scans row-major and respects screens', () => {
    expect(findFreeSpot(pg, 12, 4)).toEqual({ x: 0, y: 4, w: 12, h: 4 });
    const full = page([panel('a', 0, 0, 24, 8)]);
    expect(findFreeSpot(full, 6, 3)).toEqual({ x: 0, y: 8, w: 6, h: 3 });
  });
  it('nudge moves, resizes, jumps screens, and refuses collisions', () => {
    const p2 = page([panel('a', 0, 0, 6, 4), panel('b', 6, 0, 6, 4)]);
    expect(nudge(p2, p2.panels[0], 1, 0)).toBeNull();
    expect(nudge(p2, p2.panels[0], 0, 1)).toEqual({ x: 0, y: 1, w: 6, h: 4 });
    expect(nudge(p2, p2.panels[0], 0, 1, true)).toEqual({ x: 0, y: 0, w: 6, h: 5 });
    const p3 = page([panel('a', 0, 4, 6, 4)]);
    expect(nudge(p3, p3.panels[0], 0, 1)).toEqual({ x: 0, y: 8, w: 6, h: 4 });
  });
  it('layoutIssues mirrors validate.cpp checks', () => {
    const bad = page([panel('a', 0, 0, 12, 4), panel('b', 6, 2, 12, 4), panel('c', 0, 6, 4, 4), panel('d', 20, 8, 8, 2)]);
    const issues = layoutIssues(bad).map((i) => `${i.panelId}:${i.message}`);
    expect(issues).toEqual(expect.arrayContaining(['b:overlap', 'c:cross_screen', 'd:columns']));
  });
});
