// Screen canvas: a page is exactly one 552×768 device screen of 8 grid rows.
// Panels are drawn like the e-ink renderer (DevicePanel) at device pixels
// inside a CSS-scaled container. Pointer-based move / 8-handle resize with
// grid snapping; overlapped panels are pushed aside (model/layout.ts) and the
// result is previewed live while dragging.
import { forwardRef, useImperativeHandle, useMemo, useRef, useState, type KeyboardEvent, type PointerEvent as RPointerEvent } from 'react';
import { useI18n } from '../../i18n';
import { CONTENT, STATUS_LINE_H, STATUS_LINE_Y, GAP_X, GAP_Y, PAGER_ZONE_W, ROWS_PER_SCREEN, SCREEN_H, SCREEN_W, columnsOf, edge, isVisible, panelPixels, pitch, type LayoutIssue } from '../../model/grid';
import { HANDLES, nudgePlacement, resizeRect, resolvePlacement, type Handle, type Placement } from '../../model/layout';
import type { Bundle, GridRect, Page, Panel } from '../../model/types';
import type { ObservedMetrics } from '../../model/metrics';
import { DevicePanel } from './DevicePanel';

export const NEW_ID = '__new__';

interface DragState {
  id: string;
  mode: 'move' | Handle;
  pointerId: number;
  startX: number;
  startY: number;
  /** pointer offset inside the panel at grab time (device px) */
  offX: number;
  offY: number;
  origin: GridRect;
  candidate: GridRect;
  placement: Placement | null;
  moved: boolean;
  /** current free-floating top-left in canvas px (move mode) */
  fx: number;
  fy: number;
}

export interface CanvasHandle {
  /** Convert a client point to a grid rect of size w×h (null when outside the canvas). */
  toGrid: (clientX: number, clientY: number, w: number, h: number) => GridRect | null;
}

export interface CanvasProps {
  page: Page;
  bundle: Bundle | null;
  observed?: ObservedMetrics;
  scale: number;
  selectedId?: string | null;
  issues?: LayoutIssue[];
  showGrid?: boolean;
  editable?: boolean;
  /** Pending insertion from the palette (ghost + push preview). */
  insert?: GridRect | null;
  /** Position of this screen in the dashboard (status line "n/N" + ‹ › pager). */
  screenNo?: { n: number; total: number };
  sizeLabel?: (w: number, h: number) => string;
  onSelect?: (id: string | null) => void;
  onCommit?: (placement: Placement, via: 'pointer' | 'keyboard', activeId: string) => void;
  onBlocked?: () => void;
  onDelete?: (id: string) => void;
  onDuplicate?: (id: string) => void;
}

const sameRect = (a: GridRect, b: GridRect) => a.x === b.x && a.y === b.y && a.w === b.w && a.h === b.h;

export const Canvas = forwardRef<CanvasHandle, CanvasProps>(function Canvas(
  { page, bundle, observed, scale, selectedId, issues = [], showGrid = true, editable, insert, screenNo, sizeLabel, onSelect, onCommit, onBlocked, onDelete, onDuplicate },
  ref,
) {
  const { t } = useI18n();
  const columns = columnsOf(page);
  const rootRef = useRef<HTMLDivElement>(null);
  const [drag, setDrag] = useState<DragState | null>(null);
  const dragRef = useRef<DragState | null>(null);
  const issueIds = useMemo(() => new Set(issues.map((i) => i.panelId)), [issues]);
  const p = pitch(columns);

  const insertPlacement = useMemo(() => (insert ? resolvePlacement(page, NEW_ID, insert) : null), [insert, page]);
  const live = drag?.placement ?? insertPlacement;
  const gridOf = (panel: Panel) => live?.grids[panel.id] ?? panel.grid;

  const visible = page.panels.filter(isVisible);

  const toCanvas = (clientX: number, clientY: number) => {
    const r = rootRef.current!.getBoundingClientRect();
    return { x: (clientX - r.left) / scale, y: (clientY - r.top) / scale };
  };
  /** Snap a free top-left (canvas px) to a legal grid rect on the screen. */
  const snap = (left: number, top: number, w: number, h: number): GridRect => {
    const hh = Math.min(h, ROWS_PER_SCREEN);
    const ww = Math.min(w, columns);
    const row = Math.max(0, Math.min(ROWS_PER_SCREEN - hh, Math.round((top - CONTENT.y) / p.row)));
    const col = Math.max(0, Math.min(columns - ww, Math.round((left - CONTENT.x) / p.col)));
    return { x: col, y: row, w: ww, h: hh };
  };

  useImperativeHandle(ref, () => ({
    toGrid: (cx, cy, w, h) => {
      if (!rootRef.current) return null;
      const r = rootRef.current.getBoundingClientRect();
      if (cx < r.left || cx > r.right || cy < r.top || cy > r.bottom) return null;
      const pt = toCanvas(cx, cy);
      const wpx = w * p.col - GAP_X;
      const hpx = h * p.row - GAP_Y;
      return snap(pt.x - wpx / 2, pt.y - Math.min(hpx / 2, 40), w, h);
    },
  }));

  const begin = (e: RPointerEvent, panel: Panel, mode: DragState['mode']) => {
    if (!editable || e.button > 0) return;
    e.stopPropagation();
    onSelect?.(panel.id);
    rootRef.current?.setPointerCapture(e.pointerId);
    const px = panelPixels(panel.grid, columns);
    const pt = toCanvas(e.clientX, e.clientY);
    const left = px.x;
    const top = px.y;
    const st: DragState = { id: panel.id, mode, pointerId: e.pointerId, startX: pt.x, startY: pt.y, offX: pt.x - left, offY: pt.y - top, origin: panel.grid, candidate: panel.grid, placement: null, moved: false, fx: left, fy: top };
    dragRef.current = st;
    setDrag(st);
  };

  const move = (e: RPointerEvent) => {
    const st = dragRef.current;
    if (!st || e.pointerId !== st.pointerId) return;
    const pt = toCanvas(e.clientX, e.clientY);
    const dx = pt.x - st.startX;
    const dy = pt.y - st.startY;
    if (!st.moved && Math.hypot(dx, dy) < 4) return;
    let cand: GridRect;
    let fx = st.fx;
    let fy = st.fy;
    if (st.mode === 'move') {
      fx = pt.x - st.offX;
      fy = pt.y - st.offY;
      cand = snap(fx, fy, st.origin.w, st.origin.h);
    } else {
      cand = resizeRect(st.origin, st.mode, Math.round(dx / p.col), Math.round(dy / p.row), columns);
    }
    let placement = st.placement;
    if (!st.moved || !sameRect(cand, st.candidate)) placement = resolvePlacement(page, st.id, cand);
    const next = { ...st, candidate: cand, placement, moved: true, fx, fy };
    dragRef.current = next;
    setDrag(next);
  };

  const end = (e: RPointerEvent) => {
    const st = dragRef.current;
    if (!st || e.pointerId !== st.pointerId) return;
    dragRef.current = null;
    setDrag(null);
    if (!st.moved || sameRect(st.candidate, st.origin)) return;
    if (st.placement?.ok) onCommit?.(st.placement, 'pointer', st.id);
    else onBlocked?.();
  };

  const onKey = (e: KeyboardEvent, panel: Panel) => {
    if (!editable) return;
    const dirs: Record<string, [number, number]> = { ArrowLeft: [-1, 0], ArrowRight: [1, 0], ArrowUp: [0, -1], ArrowDown: [0, 1] };
    const d = dirs[e.key];
    if (d) {
      e.preventDefault();
      let r: Placement | null;
      if (e.shiftKey) {
        const g = resizeRect(panel.grid, 'se', d[0], d[1], columns);
        r = sameRect(g, panel.grid) ? null : resolvePlacement(page, panel.id, g);
        if (r && !r.ok) r = null;
      } else r = nudgePlacement(page, panel.id, d[0], d[1]);
      if (r) onCommit?.(r, 'keyboard', panel.id);
      else onBlocked?.();
    } else if (e.key === 'Delete' || e.key === 'Backspace') {
      e.preventDefault();
      onDelete?.(panel.id);
    } else if (e.key === 'Escape') {
      onSelect?.(null);
      (e.currentTarget as HTMLElement).blur();
    } else if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'd') {
      e.preventDefault();
      onDuplicate?.(panel.id);
    }
  };

  const height = SCREEN_H;
  const ghostRect = drag?.moved && drag.mode === 'move' ? drag.candidate : insert ?? null;
  const ghost = ghostRect ? panelPixels(ghostRect, columns) : null;
  const ghostOk = drag ? !!drag.placement?.ok : !!insertPlacement?.ok;
  const labelFont = 13 / Math.max(scale, 0.3);

  const gridSvg = useMemo(() => {
    if (!showGrid) return null;
    // One tinted rect per grid cell: shows the slots without drawing lines through gutters.
    let d = '';
    for (let r = 0; r < ROWS_PER_SCREEN; r++) {
      const y0 = CONTENT.y + edge(r, ROWS_PER_SCREEN, CONTENT.h, GAP_Y);
      const y1 = CONTENT.y + edge(r + 1, ROWS_PER_SCREEN, CONTENT.h, GAP_Y) - GAP_Y;
      for (let c = 0; c < columns; c++) {
        const x0 = CONTENT.x + edge(c, columns, CONTENT.w, GAP_X);
        const x1 = CONTENT.x + edge(c + 1, columns, CONTENT.w, GAP_X) - GAP_X;
        d += `M${x0} ${y0}h${Math.max(1, x1 - x0)}v${y1 - y0}h${-Math.max(1, x1 - x0)}z`;
      }
    }
    return (
      <svg width={SCREEN_W} height={SCREEN_H} style={{ position: 'absolute', left: 0, top: 0, pointerEvents: 'none' }} aria-hidden>
        <path d={d} fill="rgba(62,92,245,.035)" />
      </svg>
    );
  }, [columns, showGrid]);

  return (
    <div className="canvas" style={{ width: SCREEN_W * scale, height: height * scale }} onPointerDown={() => editable && onSelect?.(null)}>
      <div
        ref={rootRef}
        style={{ position: 'absolute', left: 0, top: 0, width: SCREEN_W, height, transform: `scale(${scale})`, transformOrigin: '0 0' }}
        onPointerMove={move}
        onPointerUp={end}
        onPointerCancel={end}
      >
        <div className="screen" style={{ top: 0, width: SCREEN_W, height: SCREEN_H }}>
          {(showGrid || drag || insert) && gridSvg}
          <div style={{ position: 'absolute', left: CONTENT.x, top: STATUS_LINE_Y, width: CONTENT.w, height: 1, background: '#000' }} />
          {/* Device status line (dashboard_runtime drawStatus): state, updated time, n/N and the ‹ › pager zones. */}
          <div
            aria-hidden
            style={{ position: 'absolute', left: CONTENT.x + (screenNo && screenNo.total > 1 ? 22 : 12), top: STATUS_LINE_Y + 1, width: CONTENT.w - (screenNo && screenNo.total > 1 ? 44 : 24), height: STATUS_LINE_H - 1, display: 'flex', alignItems: 'center', gap: 8, fontSize: 24, lineHeight: 1, color: '#000', opacity: 0.45, pointerEvents: 'none', whiteSpace: 'nowrap', overflow: 'hidden' }}
          >
            <span style={{ width: 12, height: 12, borderRadius: 6, background: '#000', flex: 'none' }} />
            <span style={{ flex: 1 }}>{t('editor.status_line_state')}</span>
            <span>
              {t('editor.status_line_updated')}
              {screenNo && screenNo.total > 1 ? ` · ${screenNo.n}/${screenNo.total}` : ''}
            </span>
          </div>
          {screenNo && screenNo.total > 1 && (
            <>
              <div className="pager-zone" title={t('editor.pager_prev')} style={{ left: CONTENT.x, top: STATUS_LINE_Y + 1, width: PAGER_ZONE_W, height: STATUS_LINE_H - 1 }}>
                ‹
              </div>
              <div className="pager-zone right" title={t('editor.pager_next')} style={{ left: CONTENT.x + CONTENT.w - PAGER_ZONE_W, top: STATUS_LINE_Y + 1, width: PAGER_ZONE_W, height: STATUS_LINE_H - 1 }}>
                ›
              </div>
            </>
          )}
        </div>
        {visible.map((panel) => {
          const isDrag = drag?.id === panel.id && drag.moved;
          const g = isDrag && drag.mode !== 'move' ? drag.candidate : gridOf(panel);
          const px = panelPixels(g, columns);
          let left = px.x;
          let top = px.y;
          if (isDrag && drag.mode === 'move') {
            left = drag.fx;
            top = drag.fy;
          }
          const pushed = !!live?.moved.includes(panel.id);
          const selected = panel.id === selectedId;
          const cls = ['panel-box', selected ? 'selected' : '', isDrag ? (drag.mode === 'move' ? 'dragging no-anim' : 'no-anim') : '', pushed ? 'pushed' : '', issueIds.has(panel.id) ? 'issue' : '', editable ? '' : 'static'].filter(Boolean).join(' ');
          return (
            <div
              key={panel.id}
              className={cls}
              style={{ left, top, width: px.w, height: px.h, cursor: editable ? undefined : 'default' }}
              tabIndex={editable ? 0 : -1}
              role={editable ? 'button' : undefined}
              aria-label={panel.title || panel.id}
              aria-pressed={editable ? selected : undefined}
              onPointerDown={(e) => begin(e, panel, 'move')}
              onKeyDown={(e) => onKey(e, panel)}
              onFocus={() => editable && !selected && onSelect?.(panel.id)}
            >
              <DevicePanel panel={panel} bundle={bundle} observed={observed} w={px.w} h={px.h} />
              {editable && selected && !(isDrag && drag.mode === 'move') && HANDLES.map((h) => <div key={h} className={`handle ${h}`} onPointerDown={(e) => begin(e, panel, h)} />)}
              {isDrag && drag.mode !== 'move' && sizeLabel && <div className="size-tag" style={{ fontSize: labelFont, bottom: -labelFont * 2.1 }}>{sizeLabel(g.w, g.h)}</div>}
            </div>
          );
        })}
        {ghost && (
          <div className={`panel-ghost${ghostOk ? '' : ' bad'}`} style={{ left: ghost.x, top: ghost.y, width: ghost.w, height: ghost.h }}>
            {sizeLabel && ghostRect && <span className="ghost-tag" style={{ fontSize: labelFont, top: -labelFont * 2 }}>{sizeLabel(ghostRect.w, ghostRect.h)}</span>}
          </div>
        )}
      </div>
    </div>
  );
});
