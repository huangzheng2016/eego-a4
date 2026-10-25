import { useCallback, useEffect, useRef, useState } from 'react';
import * as api from '../api';
import { useI18n } from '../i18n';
import { errorText } from '../state/errors';
import { Button, Card, Checkbox, Spinner } from '../ui';

/** Live e-ink frame from GET /display/snapshot.bmp (1-bit 552×768 BMP). */
export function PreviewPage() {
  const { t } = useI18n();
  const [url, setUrl] = useState<string | null>(null);
  const [err, setErr] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const [auto, setAuto] = useState(false);
  const [at, setAt] = useState<Date | null>(null);
  const urlRef = useRef<string | null>(null);

  const load = useCallback(async () => {
    setBusy(true);
    try {
      const blob = await api.getDisplaySnapshot();
      const next = URL.createObjectURL(blob);
      if (urlRef.current) URL.revokeObjectURL(urlRef.current);
      urlRef.current = next;
      setUrl(next);
      setErr(null);
      setAt(new Date());
    } catch (e) {
      setErr(e instanceof api.ApiError && (e.status === 404 || e.status === 503) ? t('preview.unavailable') : errorText(e, t));
    } finally {
      setBusy(false);
    }
  }, [t]);

  useEffect(() => {
    void load();
    return () => {
      if (urlRef.current) URL.revokeObjectURL(urlRef.current);
    };
  }, [load]);
  useEffect(() => {
    if (!auto) return;
    const id = setInterval(() => document.visibilityState === 'visible' && void load(), 30000);
    return () => clearInterval(id);
  }, [auto, load]);

  return (
    <div className="page stack">
      <div className="page-head">
        <h1 className="grow">{t('preview.title')}</h1>
        <Checkbox label={t('preview.auto')} checked={auto} onChange={setAuto} />
        <Button loading={busy} onClick={() => void load()}>
          {t('common.refresh')}
        </Button>
      </div>
      <div className="dim">
        {t('preview.hint')}
        {at && ` · ${at.toLocaleTimeString()}`}
      </div>
      {err && <div className="notice warn">{err}</div>}
      <Card>
        <div style={{ display: 'flex', justifyContent: 'center' }}>
          {url ? <img className="snapshot" src={url} width={552} height={768} alt={t('preview.title')} /> : !err && <Spinner />}
        </div>
      </Card>
    </div>
  );
}
