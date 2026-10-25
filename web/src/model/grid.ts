// Grid geometry shared with the firmware renderer.
// Source of truth: dashboard_core/model.h (kGridRowsPerScreen, gridFitsScreen)
// and dashboard_ui/src/layout.cpp (proportional edge mapping).
// TODO(api): the renderer mapping is being finalised by the firmware team; keep
// all screen constants here so they can be adjusted in one place.
import type { GridRect, Page, Panel } from './types';

export const SCREEN_W = 552;
export const SCREEN_H = 768;
/**
 * Dashboard area: geometry.h kDashboardContent (28, 28, 496×712) minus the
 * 22 px status line the runtime reserves at the bottom (dashboard_runtime.cpp
 * kStatusLineY = 718), i.e. what buildPageLayout() actually receives.
 */
export const CONTENT = { x: 28, y: 28, w: 496, h: 690 } as const;
export const STATUS_LINE_Y = 718;
export const GAP_X = 8;
export const GAP_Y = 8;
export const ROWS_PER_SCREEN = 8;
export const DEFAULT_COLUMNS = 24;
export const MAX_COLUMNS = 64;

export interface PixelRect {
  x: number;
  y: number;
  w: number;
  h: number;
}

export function columnsOf(page: Pick<Page, 'grid_columns'>): number {
  const c = page.grid_columns ?? DEFAULT_COLUMNS;
  return c >= 1 && c <= MAX_COLUMNS ? Math.floor(c) : DEFAULT_COLUMNS;
}

/** layout.cpp edge(): boundary b of n cells over `length` px with `gap` gutters. */
export function edge(boundary: number, cells: number, length: number, gap: number): number {
  return Math.floor((boundary * (length + gap)) / cells);
}

export function screenIndex(g: GridRect): number {
  const screen = Math.floor(g.y / ROWS_PER_SCREEN);
  const row = g.y % ROWS_PER_SCREEN;
  return row + g.h > ROWS_PER_SCREEN && row !== 0 ? screen + 1 : screen;
}

export function screenRow(g: GridRect): number {
  return screenIndex(g) === Math.floor(g.y / ROWS_PER_SCREEN) ? g.y % ROWS_PER_SCREEN : 0;
}

export function fitsScreen(g: GridRect): boolean {
  return g.h >= 1 && g.h <= ROWS_PER_SCREEN && (g.y % ROWS_PER_SCREEN) + g.h <= ROWS_PER_SCREEN;
}

export function overlaps(a: GridRect, b: GridRect): boolean {
  return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

export function isVisible(p: Panel): boolean {
  return p.visible !== false;
}

/** Pixel rectangle of a panel inside its screen (screen-local coordinates). */
export function panelPixels(g: GridRect, columns: number): PixelRect & { screen: number } {
  const col = Math.min(g.x, columns - 1);
  const span = Math.max(1, Math.min(g.w, columns - col));
  const row = screenRow(g);
  const rowSpan = Math.max(1, Math.min(g.h, ROWS_PER_SCREEN - row));
  const left = edge(col, columns, CONTENT.w, GAP_X);
  const right = edge(col + span, columns, CONTENT.w, GAP_X) - GAP_X;
  const top = edge(row, ROWS_PER_SCREEN, CONTENT.h, GAP_Y);
  const bottom = edge(row + rowSpan, ROWS_PER_SCREEN, CONTENT.h, GAP_Y) - GAP_Y;
  return {
    screen: screenIndex(g),
    x: CONTENT.x + left,
    y: CONTENT.y + top,
    w: Math.max(1, right - left),
    h: Math.max(1, bottom - top),
  };
}

/** Width of one column pitch / one row pitch in screen pixels. */
export function pitch(columns: number): { col: number; row: number } {
  return { col: (CONTENT.w + GAP_X) / columns, row: (CONTENT.h + GAP_Y) / ROWS_PER_SCREEN };
}

/** Number of screens a page uses (at least 1). */
export function screenCount(page: Page): number {
  let last = 0;
  for (const p of page.panels) if (isVisible(p)) last = Math.max(last, screenIndex(p.grid));
  return last + 1;
}

/**
 * Normalise a candidate rectangle: integer, size within bounds, inside the
 * columns, and inside exactly one screen. A rect crossing a screen boundary is
 * pulled back so its bottom sits on the boundary (or pushed to the next
 * screen when `preferNext` is set, i.e. the pointer is below the boundary).
 */
export function clampRect(r: GridRect, columns: number, preferNext = false): GridRect {
  const w = Math.max(1, Math.min(columns, Math.round(r.w)));
  const h = Math.max(1, Math.min(ROWS_PER_SCREEN, Math.round(r.h)));
  const x = Math.max(0, Math.min(columns - w, Math.round(r.x)));
  let y = Math.max(0, Math.round(r.y));
  const row = y % ROWS_PER_SCREEN;
  if (row + h > ROWS_PER_SCREEN) {
    const screenTop = y - row;
    y = preferNext ? screenTop + ROWS_PER_SCREEN : screenTop + ROWS_PER_SCREEN - h;
  }
  return { x, y, w, h };
}

/** Clamp a resize: keep the top-left corner, cap size to the columns and screen. */
export function clampResize(r: GridRect, columns: number): GridRect {
  const x = Math.max(0, Math.min(columns - 1, Math.round(r.x)));
  const y = Math.max(0, Math.round(r.y));
  const w = Math.max(1, Math.min(columns - x, Math.round(r.w)));
  const h = Math.max(1, Math.min(ROWS_PER_SCREEN - (y % ROWS_PER_SCREEN), Math.round(r.h)));
  return { x, y, w, h };
}

export function collides(r: GridRect, panels: Panel[], ignoreId?: string): Panel | undefined {
  return panels.find((p) => p.id !== ignoreId && isVisible(p) && overlaps(r, p.grid));
}

/** True when `r` is a legal placement for panel `id` on `page`. */
export function canPlace(page: Page, r: GridRect, id?: string): boolean {
  const columns = columnsOf(page);
  return (
    r.x >= 0 &&
    r.y >= 0 &&
    r.w >= 1 &&
    r.x + r.w <= columns &&
    fitsScreen(r) &&
    !collides(r, page.panels, id)
  );
}

/** Snap a drag delta (in screen px) to grid units. */
export function snapDelta(dxPx: number, dyPx: number, columns: number): { dx: number; dy: number } {
  const p = pitch(columns);
  return { dx: Math.round(dxPx / p.col), dy: Math.round(dyPx / p.row) };
}

/** First free spot of size w×h, scanning row-major from `startRow`. */
export function findFreeSpot(
  page: Page,
  w: number,
  h: number,
  startRow = 0,
  ignoreId?: string,
  maxScreens = 32,
): GridRect | null {
  const columns = columnsOf(page);
  const ww = Math.max(1, Math.min(columns, w));
  const hh = Math.max(1, Math.min(ROWS_PER_SCREEN, h));
  for (let y = startRow; y < maxScreens * ROWS_PER_SCREEN; y++) {
    for (let x = 0; x + ww <= columns; x++) {
      const r = { x, y, w: ww, h: hh };
      if (fitsScreen(r) && !collides(r, page.panels, ignoreId)) return r;
    }
  }
  return null;
}

/**
 * Keyboard nudge: move (or resize with `resize`) by one cell. Returns the new
 * rect, or null when the result would be illegal.
 */
export function nudge(page: Page, panel: Panel, dx: number, dy: number, resize = false): GridRect | null {
  const g = panel.grid;
  const next = resize ? { ...g, w: g.w + dx, h: g.h + dy } : { ...g, x: g.x + dx, y: g.y + dy };
  if (!resize && dy !== 0 && !fitsScreen(next)) {
    // Jump across the screen boundary instead of refusing.
    const screen = Math.floor(g.y / ROWS_PER_SCREEN);
    next.y = dy > 0 ? (screen + 1) * ROWS_PER_SCREEN : screen * ROWS_PER_SCREEN - g.h;
    if (next.y < 0) return null;
  }
  return canPlace(page, next, panel.id) ? next : null;
}

export interface LayoutIssue {
  panelId: string;
  message: 'cross_screen' | 'too_tall' | 'overlap' | 'columns';
  otherId?: string;
}

/** Client-side mirror of validate.cpp layout checks. */
export function layoutIssues(page: Page): LayoutIssue[] {
  const columns = columnsOf(page);
  const out: LayoutIssue[] = [];
  const visible = page.panels.filter(isVisible);
  visible.forEach((p, i) => {
    const g = p.grid;
    if (g.x + g.w > columns) out.push({ panelId: p.id, message: 'columns' });
    if (g.h > ROWS_PER_SCREEN) out.push({ panelId: p.id, message: 'too_tall' });
    else if (!fitsScreen(g)) out.push({ panelId: p.id, message: 'cross_screen' });
    for (let j = 0; j < i; j++) {
      if (overlaps(g, visible[j].grid)) out.push({ panelId: p.id, message: 'overlap', otherId: visible[j].id });
    }
  });
  return out;
}
