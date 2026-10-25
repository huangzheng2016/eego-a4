import { useEffect, useMemo, useRef, useState, type ChangeEvent } from 'react';
import { useEnumLabel, useI18n, type DictKey } from '../../i18n';
import { useDraft } from '../../state/draft';
import { useToast } from '../../state/toast';
import { navigate } from '../../state/router';
import { Badge, Button, Empty, Icon, Spinner } from '../../ui';
import { Canvas } from './Canvas';
import { PageInspector, PanelInspector, newPanel } from './Inspector';
import { layoutIssues, SCREEN_W } from '../../model/grid';
import { BundleError, bundleToText, parseBundle, uniqueId } from '../../model/bundle';
import { locateError } from '../../model/errors';
import { PANEL_KINDS, type PanelKind } from '../../model/types';

function useFitScale(el: HTMLElement | null) {
  const [fit, setFit] = useState(0.8);
  useEffect(() => {
    if (!el) return;
    const measure = () => setFit(Math.max(0.3, Math.min(1.25, (el.clientWidth - 40) / SCREEN_W)));
    measure();
    const ro = new ResizeObserver(measure);
    ro.observe(el);
    return () => ro.disconnect();
  }, [el]);
  return fit;
}

export function DraftToolbar() {
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
      <Button size="sm" icon title={`${t('draft.undo')} (Ctrl+Z)`} disabled={!d.canUndo} onClick={d.undo}>
        <Icon name="undo" />
      </Button>
      <Button size="sm" icon title={`${t('draft.redo')} (Ctrl+Shift+Z)`} disabled={!d.canRedo} onClick={d.redo}>
        <Icon name="redo" />
      </Button>
      {d.dirty ? <Badge tone="warn">{t('draft.dirty')}</Badge> : <Badge tone="ok">{t('draft.clean')}</Badge>}
      <span className="dim">{t('draft.revision', { rev: d.draft.revision })}</span>
      {d.validity === 'valid' && <Badge tone="ok">✓ {t('draft.valid')}</Badge>}
      {d.validity === 'invalid' && <Badge tone="danger">{t('draft.invalid', { n: d.errors.length })}</Badge>}
      <span className="spacer" />
      <input ref={fileRef} type="file" accept=".json,application/json" hidden onChange={onImport} />
      <Button size="sm" title={t('draft.import')} onClick={() => fileRef.current?.click()}>
        <Icon name="upload" />
      </Button>
      <Button size="sm" title={t('draft.export')} onClick={onExport}>
        <Icon name="download" />
      </Button>
      <Button size="sm" disabled={!d.dirty || !d.configured} onClick={() => confirm(t('draft.discard_confirm')) && d.discard()}>
        {t('draft.discard')}
      </Button>
      <Button size="sm" loading={d.busy === 'validate'} onClick={() => void d.validate()}>
        {t('draft.validate')}
      </Button>
      <Button size="sm" variant="primary" loading={d.busy === 'apply'} disabled={!d.dirty && d.configured} onClick={() => void d.apply()}>
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

export function EditorPage() {
  const { t } = useI18n();
  const label = useEnumLabel();
  const toast = useToast();
  const d = useDraft();
  const [wrapEl, setWrapEl] = useState<HTMLDivElement | null>(null);
  const fit = useFitScale(wrapEl);
  const [zoom, setZoom] = useState<number | null>(null);
  const [showGrid, setShowGrid] = useState(true);
  const [addKind, setAddKind] = useState('');
  useUndoShortcuts();

  const dash = d.draft.dashboard;
  const page = dash.pages.find((p) => p.id === d.pageId);
  const issues = useMemo(() => (page ? layoutIssues(page) : []), [page]);
  const scale = zoom ?? fit;

  // Errors that point at things on other pages / not panels.
  const errorItems = useMemo(() => d.errors.map((e) => ({ e, at: locateError(d.draft, e.field) })), [d.errors, d.draft]);

  if (d.loading) {
    return (
      <div className="page">
        <Spinner /> {t('common.loading')}
      </div>
    );
  }
  if (d.loadError) {
    return (
      <div className="page col">
        <div className="notice danger">{d.loadError}</div>
        <div>
          <Button onClick={() => void d.reload()}>{t('common.retry')}</Button>
        </div>
      </div>
    );
  }

  const addPage = () => {
    const id = uniqueId('page', dash.pages.map((p) => p.id));
    d.update((b) => b.dashboard.pages.push({ id, title: `${t('editor.pages')} ${b.dashboard.pages.length + 1}`, visible: true, grid_columns: 24, row_height: 16, panels: [] }));
    d.setPageId(id);
  };
  const addPanel = (kind: PanelKind) => {
    const p = newPanel(d.draft, d.pageId, kind, t('panel.new_title'));
    if (!p) return toast('warn', t('editor.no_space'));
    d.update((b) => b.dashboard.pages.find((x) => x.id === d.pageId)!.panels.push(p));
    d.setPanelId(p.id);
  };
  return (
    <div className="editor">
      <nav className="ed-pages" aria-label={t('editor.pages')}>
        <div className="row" style={{ padding: '0 4px 6px' }}>
          <h4 className="grow">{t('editor.pages')}</h4>
          <Button size="sm" icon title={t('editor.add_page')} onClick={addPage}>
            <Icon name="plus" />
          </Button>
        </div>
        {dash.pages.map((p) => {
          const pageErrs = errorItems.filter((x) => (x.at.area === 'page' || x.at.area === 'panel') && x.at.pageId === p.id).length;
          return (
            <div key={p.id} className={`page-item${p.id === d.pageId ? ' on' : ''}`} onClick={() => d.setPageId(p.id)} role="button" tabIndex={0} onKeyDown={(e) => e.key === 'Enter' && d.setPageId(p.id)}>
              <span className="grow" title={p.id}>
                {p.title || p.id}
                {p.visible === false && <span className="dim"> ·hidden</span>}
              </span>
              {pageErrs > 0 && <Badge tone="danger">{pageErrs}</Badge>}
              {dash.default_page_id === p.id && <Badge>★</Badge>}
            </div>
          );
        })}
      </nav>

      <div className="ed-center">
        <div className="ed-toolbar">
          <DraftToolbar />
        </div>
        <div className="ed-toolbar">
          <select className="input" style={{ width: 'auto', minHeight: 28 }} value={addKind} aria-label={t('editor.add_panel')} onChange={(e) => {
            if (e.target.value) addPanel(e.target.value as PanelKind);
            setAddKind('');
          }}>
            <option value="">＋ {t('editor.add_panel')}</option>
            {PANEL_KINDS.map((k) => (
              <option key={k} value={k}>
                {label('kind', k)}
              </option>
            ))}
          </select>
          <Button size="sm" onClick={() => navigate('templates')}>
            <Icon name="grid" /> {t('editor.from_template')}
          </Button>
          <span className="spacer" />
          <label className="check dim" style={{ minHeight: 28 }}>
            <input type="checkbox" checked={showGrid} onChange={(e) => setShowGrid(e.target.checked)} /> {t('editor.grid')}
          </label>
          <div className="btn-group">
            <Button size="sm" icon title="-" onClick={() => setZoom(Math.max(0.3, +(scale - 0.1).toFixed(2)))}>
              <Icon name="zoomout" />
            </Button>
            <Button size="sm" className={zoom === null ? 'on' : ''} onClick={() => setZoom(null)}>
              {Math.round(scale * 100)}%
            </Button>
            <Button size="sm" icon title="+" onClick={() => setZoom(Math.min(2, +(scale + 0.1).toFixed(2)))}>
              <Icon name="zoomin" />
            </Button>
          </div>
        </div>
        {d.restorable === null && !d.configured && <div className="notice" style={{ margin: 8 }}>{t('draft.not_configured')}</div>}
        <div className="ed-canvas-wrap" ref={setWrapEl}>
          {page ? (
            <>
              {page.panels.length === 0 && <Empty>{t('editor.empty_page')}</Empty>}
              <Canvas
                page={page}
                scale={scale}
                editable
                extraScreen
                showGrid={showGrid}
                selectedId={d.panelId}
                issues={issues}
                screenLabel={(n) => t('editor.screen', { n })}
                onSelect={d.setPanelId}
                onChange={(id, grid, via) =>
                  d.update((b) => {
                    const p = b.dashboard.pages.find((x) => x.id === page.id)?.panels.find((x) => x.id === id);
                    if (p) p.grid = grid;
                  }, via === 'keyboard' ? `nudge:${id}` : undefined)
                }
                onBlocked={() => toast('warn', t('editor.blocked'))}
                onDelete={(id) => {
                  d.update((b) => {
                    const pg = b.dashboard.pages.find((x) => x.id === page.id)!;
                    pg.panels = pg.panels.filter((x) => x.id !== id);
                  });
                  d.setPanelId(null);
                }}
              />
            </>
          ) : (
            <Empty>—</Empty>
          )}
        </div>
      </div>

      <aside className="ed-inspector">
        {(issues.length > 0 || errorItems.length > 0) && (
          <div className="notice danger" style={{ marginBottom: 12 }}>
            <strong>{t('draft.errors_title')}</strong>
            <ul style={{ margin: '4px 0 0', paddingLeft: 18 }}>
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
                    }}
                  >
                    {e.field || '/'}
                  </a>
                  : {e.message}
                </li>
              ))}
            </ul>
          </div>
        )}
        {d.panelId ? <PanelInspector key={d.panelId} panelId={d.panelId} /> : <PageInspector />}
      </aside>
    </div>
  );
}
