import { useEffect, useMemo, useState } from 'react';
import * as api from '../api';
import { useI18n } from '../i18n';
import { useDraft } from '../state/draft';
import { useToast } from '../state/toast';
import { errorText } from '../state/errors';
import { navigate } from '../state/router';
import { addTemplateAsPages, addTemplatePanels, bundleFromPreset, parseBundle, replaceWithTemplate } from '../model/bundle';
import { columnsOf, isVisible, panelPixels, SCREEN_H, SCREEN_W } from '../model/grid';
import type { Bundle, Page } from '../model/types';
import { Badge, Button, Empty, Select, Spinner } from '../ui';
import { PanelPreview } from './editor/PanelPreview';

export interface TemplateEntry {
  id: string;
  kind: 'layout' | 'panel' | 'example';
  title: string;
  description?: string;
  bundle: Bundle;
}

/** Flatten the device template library into gallery entries. */
export function libraryEntries(lib: api.TemplateLibrary): TemplateEntry[] {
  const out: TemplateEntry[] = [];
  for (const p of lib.presets ?? []) {
    try {
      out.push({ id: p.id, kind: p.kind, title: p.title ?? p.id, description: p.description, bundle: bundleFromPreset(p) });
    } catch {
      /* skip malformed preset */
    }
  }
  for (const e of lib.examples ?? []) {
    try {
      const b = parseBundle(e.bundle);
      out.push({ id: e.id, kind: 'example', title: e.title || b.dashboard.title || e.id, bundle: b });
    } catch {
      /* skip */
    }
  }
  return out;
}

/** First screen of a page, scaled to `width` px. */
export function ScreenThumb({ page, width = 150 }: { page: Page; width?: number }) {
  const scale = width / SCREEN_W;
  const columns = columnsOf(page);
  return (
    <div style={{ position: 'relative', width, height: SCREEN_H * scale }} aria-hidden>
      <div className="screen" style={{ top: 0, width: SCREEN_W, height: SCREEN_H, transform: `scale(${scale})`, transformOrigin: '0 0' }}>
        {page.panels.filter(isVisible).map((p) => {
          const px = panelPixels(p.grid, columns);
          if (px.screen !== 0) return null;
          return (
            <div key={p.id} className="panel-box" style={{ left: px.x, top: px.y, width: px.w, height: px.h, cursor: 'default', borderWidth: 3 }}>
              <PanelPreview panel={p} compact />
            </div>
          );
        })}
      </div>
    </div>
  );
}

type Filter = 'all' | TemplateEntry['kind'];

export function TemplatesPage() {
  const { t } = useI18n();
  const toast = useToast();
  const d = useDraft();
  const [entries, setEntries] = useState<TemplateEntry[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [filter, setFilter] = useState<Filter>('all');
  const [q, setQ] = useState('');
  const [targetPage, setTargetPage] = useState(d.pageId);

  useEffect(() => {
    let alive = true;
    api
      .getTemplates()
      .then((lib) => alive && setEntries(libraryEntries(lib)))
      .catch((e) => alive && setError(e instanceof api.ApiError && e.status === 404 ? t('tpl.unavailable') : errorText(e, t)));
    return () => {
      alive = false;
    };
  }, [t]);
  useEffect(() => setTargetPage((p) => (d.draft.dashboard.pages.some((x) => x.id === p) ? p : d.pageId)), [d.pageId, d.draft.dashboard.pages]);

  const shown = useMemo(
    () =>
      (entries ?? []).filter(
        (e) => (filter === 'all' || e.kind === filter) && (!q || `${e.id} ${e.title} ${e.description ?? ''}`.toLowerCase().includes(q.toLowerCase())),
      ),
    [entries, filter, q],
  );

  const addAsPage = (e: TemplateEntry) => {
    const { bundle, pageId } = addTemplateAsPages(d.draft, e.bundle);
    d.replace(bundle);
    d.setPageId(pageId);
    toast('ok', t('tpl.added_page', { name: e.title }));
    navigate('editor');
  };
  const addPanels = (e: TemplateEntry) => {
    const { bundle, panelIds } = addTemplatePanels(d.draft, targetPage, e.bundle);
    if (!panelIds.length) return toast('warn', t('editor.no_space'));
    d.replace(bundle);
    d.setPageId(targetPage);
    toast('ok', t('tpl.added_panels', { n: panelIds.length }));
    navigate('editor');
  };
  const replaceAll = (e: TemplateEntry) => {
    if (!confirm(t('tpl.replace_confirm', { name: e.title }))) return;
    d.replace(replaceWithTemplate(d.draft, e.bundle));
    toast('ok', t('tpl.replaced'));
    navigate('editor');
  };

  return (
    <div className="page wide">
      <div className="page-head">
        <h1 className="grow">{t('tpl.title')}</h1>
        <div className="btn-group">
          {(['all', 'layout', 'panel', 'example'] as Filter[]).map((f) => (
            <Button key={f} size="sm" className={filter === f ? 'on' : ''} onClick={() => setFilter(f)}>
              {f === 'all' ? t('tpl.all') : t(`tpl.kind.${f}`)}
            </Button>
          ))}
        </div>
        <input className="input" style={{ width: 180 }} placeholder={t('tpl.search')} value={q} onChange={(e) => setQ(e.target.value)} />
        <div style={{ width: 180 }}>
          <Select label={t('tpl.target_page')} value={targetPage} options={d.draft.dashboard.pages.map((p) => ({ value: p.id, label: p.title || p.id }))} onChange={setTargetPage} />
        </div>
      </div>
      {error && <div className="notice warn">{error}</div>}
      {!entries && !error && (
        <div>
          <Spinner /> {t('common.loading')}
        </div>
      )}
      {entries && !shown.length && <Empty>{t('common.empty')}</Empty>}
      <div className="tpl-grid">
        {shown.map((e) => {
          const page = e.bundle.dashboard.pages[0];
          const sources = e.bundle.dashboard.sources.map((s) => s.id);
          return (
            <article key={`${e.kind}:${e.id}`} className="card tpl-card">
              <div className="tpl-thumb">{page && <ScreenThumb page={page} />}</div>
              <div className="tpl-meta">
                <div className="row">
                  <strong className="grow">{e.title}</strong>
                  <Badge tone={e.kind === 'example' ? 'accent' : undefined}>{t(`tpl.kind.${e.kind}`)}</Badge>
                </div>
                {e.description && <div className="dim">{e.description}</div>}
                {sources.length > 0 && <div className="dim">{t('tpl.sources_note', { list: sources.join(', ') })}</div>}
              </div>
              <div className="tpl-actions">
                {e.kind === 'panel' ? (
                  <Button size="sm" variant="primary" onClick={() => addPanels(e)}>
                    {t('tpl.add_panels')}
                  </Button>
                ) : (
                  <Button size="sm" variant="primary" onClick={() => addAsPage(e)}>
                    {t('tpl.add_page')}
                  </Button>
                )}
                {e.kind === 'panel' ? (
                  <Button size="sm" onClick={() => addAsPage(e)}>
                    {t('tpl.add_page')}
                  </Button>
                ) : (
                  <Button size="sm" onClick={() => replaceAll(e)}>
                    {t('tpl.replace')}
                  </Button>
                )}
              </div>
            </article>
          );
        })}
      </div>
    </div>
  );
}
