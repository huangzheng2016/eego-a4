import { useEffect, useMemo, useRef, useState, type ChangeEvent, type PointerEvent as RPointerEvent } from 'react';
import { useEnumLabel, useI18n, type DictKey } from '../../i18n';
import { useDraft } from '../../state/draft';
import { useToast } from '../../state/toast';
import { navigate } from '../../state/router';
import { Badge, Button, EmptyState, Icon, Segmented, Skeleton } from '../../ui';
import { RenderPreviewDialog } from './RenderPreviewDialog';
import { ScreenStrip } from './ScreenStrip';
import * as api from '../../api';
import { errorText } from '../../state/errors';
import { Canvas, type CanvasHandle } from './Canvas';
import { PageInspector, PanelInspector, newPanel } from './Inspector';
import { columnsOf, findFreeSpot, layoutIssues, MAX_SCREENS, SCREEN_W } from '../../model/grid';
import { applyPlacement, defaultSize, resolvePlacement, type Placement } from '../../model/layout';
import { addScreen, allPanelIds, BundleError, bundleToText, deviceScreenIndex, duplicateScreen, moveScreen, parseBundle, removeScreen, uniqueId } from '../../model/bundle';
import { locateError } from '../../model/errors';
import { PANEL_KINDS, type GridRect, type PanelKind } from '../../model/types';
import { LivePreview } from '../LivePreview';

function useFitScale(el: HTMLElement | null) {
  const [fit, setFit] = useState(0.8);
  useEffect(() => {
    if (!el) return;
    const measure = () => setFit(Math.max(0.3, Math.min(1, (el.clientWidth - 48) / SCREEN_W)));
    measure();
    const ro = new ResizeObserver(measure);
    ro.observe(el);
    return () => ro.disconnect();
  }, [el]);
  return fit;
}

/** Draft status + undo/redo + validate/apply; shared by editor and sources. */
export function DraftToolbar({ compact }: { compact?: boolean }) {
  const { t } = useI18n();
  const d = useDraft();
  const fileRef = useRef<HTMLInputElement>(null);
  const toast = useToast();
  const onImport = async (e: ChangeEvent<HTMLInputElement>) => {
    const f = e.target.files?.[0];
    e.target.value = '';
    if (!f) return;
    try {
      const b = parseBundle(JSON.parse(await f.text()));
      d.replace({ ...b, revision: d.draft.revision });
      toast('ok', t('draft.imported'));
    } catch (x) {
      toast('error', t('draft.import_bad', { msg: x instanceof BundleError || x instanceof Error ? x.message : String(x) }));
    }
  };
  const onExport = () => {
    const blob = new Blob([bundleToText(d.draft)], { type: 'application/json' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = `${d.draft.dashboard.id || 'dashboard'}.json`;
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  };
  return (
    <>
      <div className="btn-group">
        <Button size="sm" icon variant="ghost" title={`${t('draft.undo')} (Ctrl+Z)`} disabled={!d.canUndo} onClick={d.undo}>
          <Icon name="undo" size={16} />
        </Button>
        <Button size="sm" icon variant="ghost" title={`${t('draft.redo')} (Ctrl+Shift+Z)`} disabled={!d.canRedo} onClick={d.redo}>
          <Icon name="redo" size={16} />
        </Button>
      </div>
      {d.dirty ? (
        <Badge tone="warn" dot>
          {t('draft.dirty')}
        </Badge>
      ) : (
        <Badge tone="ok" dot>
          {t('draft.clean')}
        </Badge>
      )}
      {!compact && <span className="dim hide-sm">{t('draft.revision', { rev: d.draft.revision })}</span>}
      {d.validity === 'invalid' && <Badge tone="danger">{t('draft.invalid', { n: d.errors.length })}</Badge>}
      <span className="spacer" />
      <input ref={fileRef} type="file" accept=".json,application/json" hidden onChange={onImport} />
      <Button size="sm" icon variant="ghost" className="hide-sm" title={t('draft.import')} onClick={() => fileRef.current?.click()}>
        <Icon name="upload" size={16} />
      </Button>
      <Button size="sm" icon variant="ghost" className="hide-sm" title={t('draft.export')} onClick={onExport}>
        <Icon name="download" size={16} />
      </Button>
      {d.dirty && d.configured && (
        <Button size="sm" variant="ghost" className="hide-sm" onClick={() => confirm(t('draft.discard_confirm')) && d.discard()}>
          {t('draft.discard')}
        </Button>
      )}
      <Button size="sm" loading={d.busy === 'validate'} onClick={() => void d.validate()}>
        {d.busy !== 'validate' && <Icon name={d.validity === 'valid' ? 'ok' : 'shield'} size={15} />}
        {d.validity === 'valid' ? t('draft.valid') : t('draft.validate')}
      </Button>
      <Button size="sm" variant="primary" loading={d.busy === 'apply'} disabled={!d.dirty && d.configured} onClick={() => void d.apply()}>
        {d.busy !== 'apply' && <Icon name="send" size={15} />}
        {t('draft.apply')}
      </Button>
    </>
  );
}

export function useUndoShortcuts() {
  const d = useDraft();
  useEffect(() => {
    const on = (e: KeyboardEvent) => {
      const tag = (e.target as HTMLElement)?.tagName;
      if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') return;
      if (!(e.ctrlKey || e.metaKey)) return;
      if (e.key.toLowerCase() === 'z' && !e.shiftKey) {
        e.preventDefault();
        d.undo();
      } else if ((e.key.toLowerCase() === 'z' && e.shiftKey) || e.key.toLowerCase() === 'y') {
        e.preventDefault();
        d.redo();
      }
    };
    window.addEventListener('keydown', on);
    return () => window.removeEventListener('keydown', on);
  }, [d]);
}

interface PaletteDrag {
  kind: PanelKind;
  pointerId: number;
  x: number;
  y: number;
  sx: number;
  sy: number;
  active: boolean;
  rect: GridRect | null;
}

export function EditorPage() {
  const { t } = useI18n();
  const label = useEnumLabel();
  const toast = useToast();
  const d = useDraft();
  const [wrapEl, setWrapEl] = useState<HTMLDivElement | null>(null);
  const fit = useFitScale(wrapEl);
  const [zoom, setZoom] = useState<number | null>(null);
  const [showGrid, setShowGrid] = useState(true);
  const [side, setSide] = useState<'props' | 'live'>(() => (d.appliedAt && Date.now() - d.appliedAt < 15000 ? 'live' : 'props'));
  const [pal, setPal] = useState<PaletteDrag | null>(null);
  const palRef = useRef<PaletteDrag | null>(null);
  const canvasRef = useRef<CanvasHandle>(null);
  const seenApply = useRef(d.appliedSeq);
  const [renderSupported, setRenderSupported] = useState(true);
  const [renderOpen, setRenderOpen] = useState(false);
  const [showBusy, setShowBusy] = useState(false);
  useUndoShortcuts();

  const dash = d.draft.dashboard;
  const page = dash.pages.find((p) => p.id === d.pageId);
  const issues = useMemo(() => (page ? layoutIssues(page) : []), [page]);
  const scale = zoom ?? fit;
  const errorItems = useMemo(() => d.errors.map((e) => ({ e, at: locateError(d.draft, e.field) })), [d.errors, d.draft]);

  // Show the device's real frame next to the canvas after every apply.
  useEffect(() => {
    if (d.appliedSeq !== seenApply.current) {
      seenApply.current = d.appliedSeq;
      setSide('live');
    }
  }, [d.appliedSeq]);
  useEffect(() => {
    if (d.panelId) setSide('props');
  }, [d.panelId]);

  // Ctrl/⌘ + wheel zoom.
  useEffect(() => {
    if (!wrapEl) return;
    const on = (e: WheelEvent) => {
      if (!(e.ctrlKey || e.metaKey)) return;
      e.preventDefault();
      setZoom((z) => Math.max(0.3, Math.min(2, +((z ?? fit) * (e.deltaY < 0 ? 1.1 : 0.9)).toFixed(2))));
    };
    wrapEl.addEventListener('wheel', on, { passive: false });
    return () => wrapEl.removeEventListener('wheel', on);
  }, [wrapEl, fit]);

  if (d.loading) {
    return (
      <div className="editor">
        <aside className="ed-rail ed-rail-sec col">
          <Skeleton h={20} w="60%" />
          <Skeleton h={30} />
          <Skeleton h={30} />
        </aside>
        <div className="ed-center" style={{ alignItems: 'center', paddingTop: 60 }}>
          <Skeleton w={360} h={500} r={14} />
        </div>
        <aside className="ed-side ed-side-body col">
          <Skeleton h={20} w="50%" />
          <Skeleton h={36} />
          <Skeleton h={36} />
        </aside>
      </div>
    );
  }
  if (d.loadError) {
    return (
      <div className="page">
        <EmptyState icon="error" title={t('editor.load_failed')} action={<Button onClick={() => void d.reload()}>{t('common.retry')}</Button>}>
          {d.loadError}
        </EmptyState>
      </div>
    );
  }

  const addPage = () => {
    if (dash.pages.length >= MAX_SCREENS) return toast('warn', t('editor.max_screens', { n: MAX_SCREENS }));
    const id = uniqueId('page', dash.pages.map((p) => p.id));
    const title = t('editor.screen', { n: dash.pages.length + 1 });
    d.update((b) => void addScreen(b.dashboard, title, id));
    d.setPageId(id);
  };
  const duplicatePage = () => {
    if (!page) return;
    if (dash.pages.length >= MAX_SCREENS) return toast('warn', t('editor.max_screens', { n: MAX_SCREENS }));
    const id = uniqueId(page.id, dash.pages.map((p) => p.id));
    const title = `${page.title || page.id} ${t('editor.copy_suffix')}`;
    d.update((b) => void duplicateScreen(b.dashboard, page.id, title, id));
    d.setPageId(id);
  };
  const deletePage = () => {
    if (!page || dash.pages.length <= 1) return;
    if (!confirm(t('editor.delete_page_confirm', { name: page.title || page.id }))) return;
    const index = dash.pages.indexOf(page);
    d.update((b) => void removeScreen(b.dashboard, page.id));
    const next = dash.pages.filter((p) => p.id !== page.id)[Math.max(0, index - 1)];
    if (next) d.setPageId(next.id);
  };
  const showOnDevice = async () => {
    if (!page) return;
    const index = deviceScreenIndex(dash, page.id);
    if (index < 0) return toast('warn', t('editor.show_hidden'));
    setShowBusy(true);
    try {
      const r = await api.setDeviceScreen({ index });
      toast(d.dirty ? 'info' : 'ok', t(d.dirty ? 'editor.shown_on_device_dirty' : 'editor.shown_on_device', { n: r.index + 1, total: r.count }));
    } catch (e) {
      toast('error', e instanceof api.ApiError && e.status === 409 ? t('editor.show_no_dashboard') : errorText(e, t));
    } finally {
      setShowBusy(false);
    }
  };
  const addPanel = (kind: PanelKind) => {
    const p = newPanel(d.draft, d.pageId, kind, t(`kind.${kind}` as DictKey));
    if (!p) return toast('warn', t('editor.no_space'));
    d.update((b) => b.dashboard.pages.find((x) => x.id === d.pageId)!.panels.push(p));
    d.setPanelId(p.id);
  };
  const insertPanel = (kind: PanelKind, rect: GridRect) => {
    if (!page) return;
    const p = newPanel(d.draft, d.pageId, kind, t(`kind.${kind}` as DictKey), rect);
    if (!p) return;
    const placement = resolvePlacement(page, p.id, rect);
    if (!placement.ok) return toast('warn', t('editor.no_space'));
    d.update((b) => {
      const pg = b.dashboard.pages.find((x) => x.id === page.id)!;
      pg.panels.push(p);
      applyPlacement(pg, placement);
    });
    d.setPanelId(p.id);
    if (placement.moved.length) toast('info', t('editor.pushed', { n: placement.moved.length }));
  };
  const commit = (placement: Placement, via: 'pointer' | 'keyboard', id: string) => {
    d.update((b) => {
      const pg = b.dashboard.pages.find((x) => x.id === d.pageId);
      if (pg) applyPlacement(pg, placement);
    }, via === 'keyboard' ? `nudge:${id}` : undefined);
  };
  const deletePanel = (id: string) => {
    d.update((b) => {
      for (const pg of b.dashboard.pages) {
        pg.panels = pg.panels.filter((x) => x.id !== id);
        for (const x of pg.panels) if (x.linked_panel_ids) x.linked_panel_ids = x.linked_panel_ids.filter((l) => l !== id);
      }
    });
    d.setPanelId(null);
  };
  const duplicatePanel = (id: string) => {
    if (!page) return;
    const src = page.panels.find((x) => x.id === id);
    if (!src) return;
    const spot = findFreeSpot(page, src.grid.w, src.grid.h, src.grid.y);
    if (!spot) return toast('warn', t('editor.no_space'));
    const copy = { ...structuredClone(src), id: uniqueId(src.id, allPanelIds(dash)), grid: spot };
    d.update((b) => b.dashboard.pages.find((x) => x.id === page.id)!.panels.push(copy));
    d.setPanelId(copy.id);
  };

  // ---------------------------------------------------- palette drag & drop
  const palDown = (e: RPointerEvent, kind: PanelKind) => {
    if (e.button > 0) return;
    (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
    const st: PaletteDrag = { kind, pointerId: e.pointerId, x: e.clientX, y: e.clientY, sx: e.clientX, sy: e.clientY, active: false, rect: null };
    palRef.current = st;
    setPal(st);
  };
  const palMove = (e: RPointerEvent) => {
    const st = palRef.current;
    if (!st || st.pointerId !== e.pointerId) return;
    const active = st.active || Math.hypot(e.clientX - st.sx, e.clientY - st.sy) > 6;
    let rect: GridRect | null = null;
    if (active && page) {
      const size = defaultSize(st.kind, columnsOf(page));
      rect = canvasRef.current?.toGrid(e.clientX, e.clientY, size.w, size.h) ?? null;
    }
    const next = { ...st, x: e.clientX, y: e.clientY, active, rect };
    palRef.current = next;
    setPal(next);
  };
  const palUp = (e: RPointerEvent) => {
    const st = palRef.current;
    if (!st || st.pointerId !== e.pointerId) return;
    palRef.current = null;
    setPal(null);
    if (!st.active) addPanel(st.kind);
    else if (st.rect) insertPanel(st.kind, st.rect);
  };

  const pageErrCount = (id: string) => errorItems.filter((x) => (x.at.area === 'page' || x.at.area === 'panel') && x.at.pageId === id).length;

  return (
    <div className="editor">
      <aside className="ed-rail">
        <section className="ed-rail-sec">
          <div className="ed-rail-head">
            <h4 className="grow">{t('editor.palette')}</h4>
          </div>
          <div className="palette">
            {PANEL_KINDS.map((k) => (
              <div
                key={k}
                className="pal-item"
                role="button"
                tabIndex={0}
                title={t('editor.palette_hint')}
                onPointerDown={(e) => palDown(e, k)}
                onPointerMove={palMove}
                onPointerUp={palUp}
                onPointerCancel={() => {
                  palRef.current = null;
                  setPal(null);
                }}
                onKeyDown={(e) => e.key === 'Enter' && addPanel(k)}
              >
                <Icon name={`k-${k}`} size={22} />
                {label('kind', k)}
              </div>
            ))}
          </div>
          <p className="dim" style={{ marginTop: 8 }}>{t('editor.palette_hint')}</p>
        </section>
        <section className="ed-rail-sec">
          <Button size="sm" style={{ width: '100%' }} onClick={() => navigate('templates')}>
            <Icon name="templates" size={16} /> {t('editor.from_template')}
          </Button>
        </section>
      </aside>

      <div className="ed-center">
        <div className="ed-toolbar">
          <DraftToolbar compact />
        </div>
        <ScreenStrip
          pages={dash.pages}
          currentId={d.pageId}
          defaultId={dash.default_page_id}
          errorCount={pageErrCount}
          onSelect={d.setPageId}
          onAdd={addPage}
          onDuplicate={duplicatePage}
          onDelete={deletePage}
          onMove={(from, to) => d.update((b) => moveScreen(b.dashboard, from, to))}
          onRename={(id, title) => {
            const cur = dash.pages.find((p) => p.id === id);
            if (cur && (cur.title ?? '') !== title) d.update((b) => (b.dashboard.pages.find((p) => p.id === id)!.title = title));
          }}
          onShowOnDevice={d.configured ? () => void showOnDevice() : null}
          showBusy={showBusy}
        />
        <div className="ed-toolbar" style={{ minHeight: 44, paddingTop: 6, paddingBottom: 6 }}>
          <strong className="truncate" style={{ maxWidth: 260 }}>{page?.title || page?.id}</strong>
          {page && <span className="dim hide-sm">{t('editor.grid_info', { c: columnsOf(page) })}</span>}
          <span className="spacer" />
          <span className="dim demo-key hide-sm" title={t('editor.demo_hint')}>
            <i /> {t('editor.demo')}
          </span>
          {renderSupported && (
            <Button size="sm" variant="ghost" disabled={!page} onClick={() => setRenderOpen(true)} title={t('editor.render_hint')}>
              <Icon name="device" size={16} /> <span className="hide-sm">{t('editor.render')}</span>
            </Button>
          )}
          <Button size="sm" variant="ghost" className={showGrid ? 'on' : ''} onClick={() => setShowGrid(!showGrid)} title={t('editor.grid')}>
            <Icon name="grid" size={16} /> <span className="hide-sm">{t('editor.grid')}</span>
          </Button>
          <div className="btn-group">
            <Button size="sm" icon variant="ghost" title={t('editor.zoom_out')} onClick={() => setZoom(Math.max(0.3, +(scale - 0.1).toFixed(2)))}>
              <Icon name="zoomout" size={16} />
            </Button>
            <Button size="sm" variant="ghost" title={t('editor.fit')} onClick={() => setZoom(null)} style={{ minWidth: 56 }}>
              {Math.round(scale * 100)}%
            </Button>
            <Button size="sm" icon variant="ghost" title={t('editor.zoom_in')} onClick={() => setZoom(Math.min(2, +(scale + 0.1).toFixed(2)))}>
              <Icon name="zoomin" size={16} />
            </Button>
          </div>
        </div>
        {!d.configured && d.restorable === null && (
          <div className="notice" style={{ margin: '10px 12px 0' }}>
            <Icon name="info" size={16} />
            <span className="grow">{t('draft.not_configured')}</span>
            <Button size="sm" onClick={() => navigate('welcome')}>
              <Icon name="wand" size={15} /> {t('wz.open')}
            </Button>
          </div>
        )}
        <div className="ed-canvas-wrap" ref={setWrapEl}>
          {page ? (
            <Canvas
              ref={canvasRef}
              page={page}
              bundle={d.draft}
              observed={d.observed}
              scale={scale}
              editable
              showGrid={showGrid}
              selectedId={d.panelId}
              issues={issues}
              insert={pal?.active ? pal.rect : null}
              screenNo={deviceScreenIndex(dash, page.id) >= 0 ? { n: deviceScreenIndex(dash, page.id) + 1, total: dash.pages.filter((p) => p.visible !== false).length } : undefined}
              sizeLabel={(w, h) => t('editor.size', { w, h })}
              onSelect={d.setPanelId}
              onCommit={(pl, via, id) => {
                commit(pl, via, id);
                if (via === 'pointer' && pl.moved.length) toast('info', t('editor.pushed', { n: pl.moved.length }));
              }}
              onBlocked={() => toast('warn', t('editor.blocked'))}
              onDelete={deletePanel}
              onDuplicate={duplicatePanel}
            />
          ) : (
            <EmptyState icon="layout" title={t('editor.no_page')} action={<Button onClick={addPage}>{t('editor.add_page')}</Button>} />
          )}
        </div>
      </div>

      <aside className="ed-side">
        <div className="ed-side-tabs">
          <Segmented
            value={side}
            onChange={setSide}
            options={[
              { value: 'props', label: t('editor.tab_props'), icon: 'sliders' },
              { value: 'live', label: t('editor.tab_live'), icon: 'device' },
            ]}
          />
        </div>
        <div className="ed-side-body">
          {side === 'live' ? (
            <LivePreview compact />
          ) : (
            <>
              {(issues.length > 0 || errorItems.length > 0) && (
                <div className="notice danger issues">
                  <Icon name="warn" size={16} />
                  <div className="grow">
                    <strong>{t('draft.errors_title')}</strong>
                    <ul>
                      {issues.map((i, k) => (
                        <li key={`i${k}`}>
                          <a href="#/editor" onClick={(e) => (e.preventDefault(), d.setPanelId(i.panelId))}>
                            {i.panelId}
                          </a>
                          : {t(`editor.layout_issue.${i.message}` as DictKey, { other: i.otherId ?? '' })}
                        </li>
                      ))}
                      {errorItems.slice(0, 12).map(({ e, at }, k) => (
                        <li key={`e${k}`}>
                          <a
                            href="#/editor"
                            onClick={(ev) => {
                              ev.preventDefault();
                              if (at.area === 'panel') {
                                d.setPageId(at.pageId);
                                setTimeout(() => d.setPanelId(at.panelId));
                              } else if (at.area === 'page') d.setPageId(at.pageId);
                              else if (at.area === 'source') navigate('sources', at.sourceId);
                              else if (e.field.includes('alert_rules')) navigate('rules');
                            }}
                          >
                            {e.field || '/'}
                          </a>
                          : {e.message}
                        </li>
                      ))}
                    </ul>
                  </div>
                </div>
              )}
              {d.panelId ? <PanelInspector key={d.panelId} panelId={d.panelId} onDelete={deletePanel} onDuplicate={duplicatePanel} /> : <PageInspector />}
            </>
          )}
        </div>
      </aside>
      {renderOpen && page && (
        <RenderPreviewDialog
          pageId={page.id}
          pageTitle={page.title || page.id}
          onClose={() => setRenderOpen(false)}
          onUnsupported={() => {
            setRenderOpen(false);
            setRenderSupported(false);
            toast('info', t('editor.render_unsupported'));
          }}
        />
      )}
      {pal?.active && (
        <div className="pal-chip" style={{ left: pal.x, top: pal.y }}>
          <Icon name={`k-${pal.kind}`} size={16} /> {label('kind', pal.kind)}
        </div>
      )}
    </div>
  );
}
