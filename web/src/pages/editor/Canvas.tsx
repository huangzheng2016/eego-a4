// Page canvas: each device screen is a 552×768 frame holding 8 grid rows.
// Panels are rendered at device pixels inside a CSS-scaled container. Own
// pointer-based drag/resize (mouse, pen, touch) with grid snapping; illegal
// placements (overlap / crossing a screen) are refused.
import { useMemo, useRef, useState, type KeyboardEvent, type PointerEvent as RPointerEvent } from 'react';
import {
  CONTENT,
  ROWS_PER_SCREEN,
  SCREEN_H,
  SCREEN_W,
  canPlace,
  clampRect,
  clampResize,
  columnsOf,
  edge,
  GAP_X,
  GAP_Y,
  isVisible,
  nudge,
  panelPixels,
  pitch,
  screenCount,
  type LayoutIssue,
} from '../../model/grid';
import type { GridRect, Page, Panel } from '../../model/types';
import { PanelPreview } from './PanelPreview';

export const SCREEN_GAP = 36;
export const LABEL_H = 22;

export function screenTop(s: number): number {
  return LABEL_H + s * (SCREEN_H + SCREEN_GAP + LABEL_H);
}
export function canvasHeight(screens: number): number {
  return screenTop(screens) - SCREEN_GAP;
}

interface DragState {
  id: string;
  mode: 'move' | 'resize';
  startX: number;
  startY: number;
  origin: GridRect;
  candidate: GridRect;
  legal: boolean;
  moved: boolean;
  pointerId: number;
}

export interface CanvasProps {
  page: Page;
  scale: number;
  selectedId?: string | null;
  issues?: LayoutIssue[];
  showGrid?: boolean;
  editable?: boolean;
  /** Render one extra empty screen below the last used one (drop target). */
  extraScreen?: boolean;
  screenLabel?: (n: number) => string;
  onSelect?: (id: string | null) => void;
  onChange?: (id: string, grid: GridRect, via: 'pointer' | 'keyboard') => void;
  onBlocked?: () => void;
  onDelete?: (id: string) => void;
  samples?: Record<string, { value: number; unit?: string }>;
}

export function Canvas({ page, scale, selectedId, issues = [], showGrid = true, editable, extraScreen, screenLabel, onSelect, onChange, onBlocked, onDelete, samples }: CanvasProps) {
  const columns = columnsOf(page);
  const [drag, setDrag] = useState<DragState | null>(null);
  const dragRef = useRef<DragState | null>(null);
  const screens = screenCount(page) + (extraScreen ? 1 : 0);
  const issueIds = useMemo(() => new Set(issues.map((i) => i.panelId)), [issues]);
  const p = pitch(columns);

  const gridLines = useMemo(() => {
    if (!showGrid) return null;
    const out: JSX.Element[] = [];
    for (let c = 1; c < columns; c++) {
      const x = CONTENT.x + edge(c, columns, CONTENT.w, GAP_X) - GAP_X / 2;
      out.push(<div key={`c${c}`} className="grid-line" style={{ left: x, top: CONTENT.y, width: 1, height: CONTENT.h }} />);
    }
    for (let r = 1; r < ROWS_PER_SCREEN; r++) {
      const y = CONTENT.y + edge(r, ROWS_PER_SCREEN, CONTENT.h, GAP_Y) - GAP_Y / 2;
      out.push(<div key={`r${r}`} className="grid-line" style={{ top: y, left: CONTENT.x, height: 1, width: CONTENT.w }} />);
    }
    return out;
  }, [columns, showGrid]);

  const begin = (e: RPointerEvent, panel: Panel, mode: DragState['mode']) => {
    if (!editable || e.button > 0) return;
    e.stopPropagation();
    onSelect?.(panel.id);
    (e.currentTarget as Element).setPointerCapture(e.pointerId);
    const st: DragState = { id: panel.id, mode, startX: e.clientX, startY: e.clientY, origin: panel.grid, candidate: panel.grid, legal: true, moved: false, pointerId: e.pointerId };
    dragRef.current = st;
    setDrag(st);
  };

  const move = (e: RPointerEvent) => {
    const st = dragRef.current;
    if (!st || e.pointerId !== st.pointerId) return;
    const dx = (e.clientX - st.startX) / scale;
    const dy = (e.clientY - st.startY) / scale;
    if (!st.moved && Math.hypot(dx, dy) < 4) return;
    let cand: GridRect;
    if (st.mode === 'resize') {
      cand = clampResize({ ...st.origin, w: st.origin.w + Math.round(dx / p.col), h: st.origin.h + Math.round(dy / p.row) }, columns);
    } else {
      const px = panelPixels(st.origin, columns);
      const topCanvas = screenTop(px.screen) + px.y + dy;
      const leftCanvas = px.x + dx;
      const stride = SCREEN_H + SCREEN_GAP + LABEL_H;
      const s = Math.max(0, Math.min(screens - 1 + (extraScreen ? 0 : 1), Math.floor((topCanvas - LABEL_H + SCREEN_GAP / 2) / stride)));
      const localY = topCanvas - screenTop(s) - CONTENT.y;
      const row = Math.max(0, Math.min(ROWS_PER_SCREEN - st.origin.h, Math.round(localY / p.row)));
      const col = Math.round((leftCanvas - CONTENT.x) / p.col);
      cand = clampRect({ x: col, y: s * ROWS_PER_SCREEN + row, w: st.origin.w, h: st.origin.h }, columns);
    }
    const next = { ...st, candidate: cand, legal: canPlace(page, cand, st.id), moved: true };
    dragRef.current = next;
    setDrag(next);
  };

  const end = (e: RPointerEvent) => {
    const st = dragRef.current;
    if (!st || e.pointerId !== st.pointerId) return;
    dragRef.current = null;
    setDrag(null);
    if (!st.moved) return;
    const c = st.candidate;
    const o = st.origin;
    if (c.x === o.x && c.y === o.y && c.w === o.w && c.h === o.h) return;
    if (st.legal) onChange?.(st.id, c, 'pointer');
    else onBlocked?.();
  };

  const onKey = (e: KeyboardEvent, panel: Panel) => {
    if (!editable) return;
    const dirs: Record<string, [number, number]> = { ArrowLeft: [-1, 0], ArrowRight: [1, 0], ArrowUp: [0, -1], ArrowDown: [0, 1] };
    const d = dirs[e.key];
    if (d) {
      e.preventDefault();
      const next = nudge(page, panel, d[0], d[1], e.shiftKey);
      if (next) onChange?.(panel.id, next, 'keyboard');
      else onBlocked?.();
    } else if (e.key === 'Delete' || e.key === 'Backspace') {
      e.preventDefault();
      onDelete?.(panel.id);
    } else if (e.key === 'Escape') {
      onSelect?.(null);
    }
  };

  const height = canvasHeight(screens);
  const ghost = drag?.moved ? panelPixels(drag.candidate, columns) : null;

  return (
    <div className="canvas" style={{ width: SCREEN_W * scale, height: height * scale }} onPointerDown={() => editable && onSelect?.(null)}>
      <div style={{ position: 'absolute', left: 0, top: 0, width: SCREEN_W, height, transform: `scale(${scale})`, transformOrigin: '0 0' }} onPointerMove={move} onPointerUp={end} onPointerCancel={end}>
        {Array.from({ length: screens }, (_, s) => (
          <div key={s}>
            {screenLabel && (
              <div className="screen-label" style={{ top: screenTop(s) - LABEL_H, fontSize: 12 / Math.max(scale, 0.35) }}>
                {screenLabel(s + 1)}
              </div>
            )}
            <div className="screen" style={{ top: screenTop(s), width: SCREEN_W, height: SCREEN_H }}>
              {showGrid && <div className="screen-content" style={{ left: CONTENT.x, top: CONTENT.y, width: CONTENT.w, height: CONTENT.h }} />}
              {gridLines}
            </div>
          </div>
        ))}
        {page.panels.filter(isVisible).map((panel) => {
          const px = panelPixels(panel.grid, columns);
          const cls = ['panel-box', panel.id === selectedId ? 'selected' : '', drag?.id === panel.id && drag.moved ? 'dragging' : '', issueIds.has(panel.id) ? 'issue' : ''].join(' ');
          return (
            <div
              key={panel.id}
              className={cls}
              style={{ left: px.x, top: screenTop(px.screen) + px.y, width: px.w, height: px.h, cursor: editable ? undefined : 'default' }}
              tabIndex={editable ? 0 : -1}
              role={editable ? 'button' : undefined}
              aria-label={panel.title || panel.id}
              aria-pressed={panel.id === selectedId}
              onPointerDown={(e) => begin(e, panel, 'move')}
              onKeyDown={(e) => onKey(e, panel)}
              onFocus={() => editable && panel.id !== selectedId && onSelect?.(panel.id)}
            >
              <PanelPreview panel={panel} sample={samples?.[panel.id]} compact={!editable} />
              {editable && panel.id === selectedId && <div className="resize-handle" onPointerDown={(e) => begin(e, panel, 'resize')} />}
            </div>
          );
        })}
        {ghost && drag && <div className={`panel-ghost${drag.legal ? '' : ' bad'}`} style={{ left: ghost.x, top: screenTop(ghost.screen) + ghost.y, width: ghost.w, height: ghost.h }} />}
      </div>
    </div>
  );
}
