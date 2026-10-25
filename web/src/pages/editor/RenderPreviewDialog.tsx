// "设备渲染预览": POST /dashboard/render-preview renders the *draft* on the
// device with its real fonts (nothing is applied). Full (4 grey levels) or
// fast (B/W) refresh look, screen pager from X-Preview-Screens and the data
// origin badge from X-Preview-Data. Validation errors go to the draft's field
// mapping (editor error list / inspector).
import { useCallback, useEffect, useRef, useState } from 'react';
import * as api from '../../api';
import { ApiError } from '../../api';
import { useI18n, type DictKey } from '../../i18n';
import { useDraft } from '../../state/draft';
import { errorText } from '../../state/errors';
import { serializeBundle } from '../../model/bundle';
import { Badge, Button, Icon, Modal, Segmented } from '../../ui';

const DATA_TONE: Record<string, 'ok' | 'warn' | 'accent' | undefined> = { live: 'ok', mixed: 'accent', bundle: 'accent', placeholder: 'warn', none: 'warn' };

export function RenderPreviewDialog({ pageId, pageTitle, onClose, onUnsupported }: { pageId: string; pageTitle: string; onClose: () => void; onUnsupported: () => void }) {
  const { t } = useI18n();
  const d = useDraft();
  const [refresh, setRefresh] = useState<api.PreviewRefresh>('full');
  const [screen, setScreen] = useState(0);
  const [result, setResult] = useState<(Omit<api.RenderPreview, 'blob'> & { url: string }) | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const urlRef = useRef<string | null>(null);
  const draftRef = useRef(d.draft);
  draftRef.current = d.draft;

  const render = useCallback(
    async (scr: number, mode: api.PreviewRefresh) => {
      setBusy(true);
      setError(null);
      try {
        const r = await api.renderPreview(serializeBundle(draftRef.current), { pageId, screen: scr, refresh: mode });
        const url = URL.createObjectURL(r.blob);
        if (urlRef.current) URL.revokeObjectURL(urlRef.current);
        urlRef.current = url;
        setResult({ page: r.page, screen: r.screen, screens: r.screens, data: r.data, url });
        setScreen(r.screen);
      } catch (e) {
        if (e instanceof ApiError && (e.status === 404 || e.status === 405)) {
          onUnsupported();
          return;
        }
        if (e instanceof ApiError && e.code === 'validation_failed') {
          d.reportErrors(e.fields);
          setError(t('draft.invalid', { n: e.fields.length || 1 }) + ' · ' + t('editor.render_fix'));
        } else if (e instanceof ApiError && e.status === 429) setError(t('editor.render_rate', { s: e.retryAfter ?? 60 }));
        else if (e instanceof ApiError && e.status === 503) setError(t('editor.render_busy'));
        else if (e instanceof ApiError && e.status === 413) setError(t('err.too_large'));
        else setError(errorText(e, t));
      } finally {
        setBusy(false);
      }
    },
    [pageId, d, t, onUnsupported],
  );

  useEffect(() => {
    void render(0, 'full');
    return () => {
      if (urlRef.current) URL.revokeObjectURL(urlRef.current);
    };
    // Render once on open; later renders are explicit.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const screens = result?.screens ?? 1;
  const go = (s: number) => void render(s, refresh);

  return (
    <Modal title={t('editor.render_title', { page: pageTitle })} onClose={onClose}>
      <div className="col" style={{ gap: 12 }}>
        <div className="row">
          <Segmented<api.PreviewRefresh>
            size="sm"
            value={refresh}
            onChange={(m) => {
              setRefresh(m);
              void render(screen, m);
            }}
            options={[
              { value: 'full', label: t('editor.render_full'), title: t('editor.render_full_hint') },
              { value: 'fast', label: t('editor.render_fast'), title: t('editor.render_fast_hint') },
            ]}
          />
          <span className="spacer" />
          {result && (
            <Badge tone={DATA_TONE[result.data]} dot>
              {result.data in DATA_TONE ? t(`editor.render_data.${result.data}` as DictKey) : result.data}
            </Badge>
          )}
        </div>
        <div className="live-frame" style={{ maxWidth: 400, margin: '0 auto', width: '100%', opacity: busy ? 0.6 : 1 }}>
          {result ? <img src={result.url} width={552} height={768} alt={t('editor.render')} /> : <div className="live-ph">{busy ? <span className="spin" /> : error}</div>}
        </div>
        <div className="row" style={{ justifyContent: 'center' }}>
          <Button size="sm" icon disabled={busy || screen <= 0} onClick={() => go(screen - 1)} aria-label={t('editor.render_prev')}>
            <Icon name="chevl" size={16} />
          </Button>
          <span className="muted" style={{ minWidth: 90, textAlign: 'center' }}>
            {t('editor.render_screen', { n: screen + 1, total: screens })}
          </span>
          <Button size="sm" icon disabled={busy || screen >= screens - 1} onClick={() => go(screen + 1)} aria-label={t('editor.render_next')}>
            <Icon name="chevr" size={16} />
          </Button>
          <Button size="sm" loading={busy} onClick={() => go(screen)}>
            {!busy && <Icon name="refresh" size={15} />} {t('editor.render_again')}
          </Button>
        </div>
        {error && result && <div className="notice warn">{error}</div>}
        {error && !result && !busy && <div className="notice danger">{error}</div>}
        <p className="dim">{t('editor.render_note')}</p>
      </div>
    </Modal>
  );
}
