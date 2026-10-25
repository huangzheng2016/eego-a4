// Editor layout operations on top of grid.ts: collision resolution for drag /
// resize / drop ("push others aside"), edge-handle resizing, and default
// panel sizes. Pure functions, unit-tested in test/layout.test.ts.
//
// Collision rule (predictable on purpose):
//   1. The active panel takes its target rectangle.
//   2. Every other visible panel is visited in reading order (y, then x).
//      If it overlaps something already settled, it is pushed straight down
//      to the first row where it still fits on the screen.
//   3. Otherwise it moves to the first free slot on the screen (row-major,
//      skipping settled panels and the panels not yet visited). When nothing
//      fits the placement fails: a page is exactly one 8-row screen.
//   Hidden panels are ignored.
import { columnsOf, fitsScreen, isVisible, overlaps, ROWS_PER_SCREEN } from './grid';
import type { GridRect, Page, PanelKind } from './types';

export interface Placement {
  ok: boolean;
  /** New rectangle for every panel whose grid is decided (active + visible others). */
  grids: Record<string, GridRect>;
  /** IDs of other panels that had to move. */
  moved: string[];
}

const hitsAny = (r: GridRect, list: GridRect[]) => list.some((o) => overlaps(r, o));
const same = (a: GridRect, b: GridRect) => a.x === b.x && a.y === b.y && a.w === b.w && a.h === b.h;

function legal(r: GridRect, columns: number): boolean {
  return r.x >= 0 && r.y >= 0 && r.w >= 1 && r.h >= 1 && r.x + r.w <= columns && fitsScreen(r);
}

/**
 * Place panel `id` at `target` (or insert a new panel when `id` is not on the
 * page) and push overlapping panels aside.
 */
export function resolvePlacement(page: Page, id: string, target: GridRect): Placement {
  const columns = columnsOf(page);
  if (!legal(target, columns)) return { ok: false, grids: {}, moved: [] };
  const others = page.panels
    .map((p, i) => ({ p, i }))
    .filter(({ p }) => p.id !== id && isVisible(p))
    .sort((a, b) => a.p.grid.y - b.p.grid.y || a.p.grid.x - b.p.grid.x || a.i - b.i)
    .map(({ p }) => p);
  const grids: Record<string, GridRect> = { [id]: { ...target } };
  const settled: GridRect[] = [target];
  const moved: string[] = [];

  for (let i = 0; i < others.length; i++) {
    const p = others[i];
    const g = { ...p.grid, w: Math.min(p.grid.w, columns), x: Math.min(p.grid.x, Math.max(0, columns - p.grid.w)) };
    let placed: GridRect | null = null;
    if (!hitsAny(g, settled)) {
      placed = g;
    } else {
      // 2. push down while it still fits on the screen
      for (let y = g.y + 1; y + g.h <= ROWS_PER_SCREEN; y++) {
        const r = { ...g, y };
        if (!hitsAny(r, settled)) {
          placed = r;
          break;
        }
      }
      // 3. first free slot on the screen, avoiding settled and not-yet-visited panels
      if (!placed) {
        const avoid = [...settled, ...others.slice(i + 1).map((o) => o.grid)];
        const h = Math.min(g.h, ROWS_PER_SCREEN);
        outer: for (let y = 0; y + h <= ROWS_PER_SCREEN; y++) {
          for (let x = 0; x + g.w <= columns; x++) {
            const r = { x, y, w: g.w, h };
            if (fitsScreen(r) && !hitsAny(r, avoid)) {
              placed = r;
              break outer;
            }
          }
        }
      }
    }
    if (!placed) return { ok: false, grids: {}, moved: [] };
    settled.push(placed);
    grids[p.id] = placed;
    if (!same(placed, p.grid)) moved.push(p.id);
  }
  return { ok: true, grids, moved };
}

/** Write a placement back onto a (mutable) page. */
export function applyPlacement(page: Page, placement: Placement): void {
  for (const p of page.panels) {
    const g = placement.grids[p.id];
    if (g) p.grid = { ...g };
  }
}

export type Handle = 'n' | 's' | 'e' | 'w' | 'ne' | 'nw' | 'se' | 'sw';
export const HANDLES: Handle[] = ['n', 's', 'e', 'w', 'ne', 'nw', 'se', 'sw'];

/**
 * Resize `origin` by dragging `handle` by (dc, dr) cells. The opposite edges
 * stay put; the result stays inside the columns and inside the screen.
 */
export function resizeRect(origin: GridRect, handle: Handle, dc: number, dr: number, columns: number): GridRect {
  let { x, y, w, h } = origin;
  const screenTop = 0;
  const screenEnd = ROWS_PER_SCREEN;
  if (handle.includes('e')) w = clamp(origin.w + dc, 1, columns - origin.x);
  if (handle.includes('w')) {
    const right = Math.min(columns, origin.x + origin.w);
    x = clamp(origin.x + dc, 0, right - 1);
    w = right - x;
  }
  if (handle.includes('s')) h = clamp(origin.h + dr, 1, screenEnd - origin.y);
  if (handle.includes('n')) {
    const bottom = Math.min(screenEnd, origin.y + origin.h);
    y = clamp(origin.y + dr, screenTop, bottom - 1);
    h = bottom - y;
  }
  return { x, y, w, h };
}

function clamp(v: number, lo: number, hi: number): number {
  return Math.max(lo, Math.min(hi, Math.round(v)));
}

/** Default footprint for a newly added panel kind. */
export function defaultSize(kind: PanelKind, columns: number): { w: number; h: number } {
  const half = Math.max(1, Math.round(columns / 2));
  switch (kind) {
    case 'chart':
    case 'table':
      return { w: columns, h: 3 };
    case 'button':
      return { w: half, h: 1 };
    case 'alert':
      return { w: columns, h: 2 };
    case 'text':
      return { w: half, h: 2 };
    default:
      return { w: half, h: 2 };
  }
}

/** Move by whole cells (keyboard nudge) with push-aside; stops at the screen edges. */
export function nudgePlacement(page: Page, id: string, dx: number, dy: number): Placement | null {
  const panel = page.panels.find((p) => p.id === id);
  if (!panel) return null;
  const columns = columnsOf(page);
  const g = panel.grid;
  const next = { ...g, x: g.x + dx, y: g.y + dy };
  if (next.x < 0 || next.x + next.w > columns || !fitsScreen(next)) return null;
  const r = resolvePlacement(page, id, next);
  return r.ok ? r : null;
}
