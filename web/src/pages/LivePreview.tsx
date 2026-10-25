// "设备实时画面": GET /display/snapshot.bmp (1-bit 552×768) shown in a device
// frame. Re-fetched automatically a few seconds after every successful apply
// (the e-ink refresh takes a moment) and optionally every 30 s.
import { useCallback, useEffect, useRef, useState } from 'react';
import * as api from '../api';
import { useI18n } from '../i18n';
import { useDraft } from '../state/draft';
import { errorText } from '../state/errors';
import { Badge, Button, Icon, Switch } from '../ui';

const AFTER_APPLY_S = [4, 12];

export function LivePreview({ compact }: { compact?: boolean }) {
  const { t } = useI18n();
  const { appliedAt } = useDraft();
  const [url, setUrl] = useState<string | null>(null);
  const [err, setErr] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const [auto, setAuto] = useState(false);
  const [at, setAt] = useState<Date | null>(null);
  const [countdown, setCountdown] = useState<{ left: number; total: number } | null>(null);
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

  // Refresh after each apply (also when mounted shortly after one): a
  // countdown to the first fetch, then a second fetch for slow refreshes.
  useEffect(() => {
    if (!appliedAt) return;
    const elapsed = (Date.now() - appliedAt) / 1000;
    const pending = AFTER_APPLY_S.filter((s) => s > elapsed);
    if (!pending.length) return;
    const timers: ReturnType<typeof setTimeout>[] = [];
    const total = AFTER_APPLY_S[0];
    const tick = () => {
      const left = Math.ceil(total - (Date.now() - appliedAt) / 1000);
      setCountdown(left > 0 ? { left, total } : null);
    };
    tick();
    const iv = setInterval(tick, 500);
    timers.push(setTimeout(() => clearInterval(iv), total * 1000 + 600));
    for (const s of pending) timers.push(setTimeout(() => void load(), (s - elapsed) * 1000));
    return () => {
      clearInterval(iv);
      timers.forEach(clearTimeout);
    };
  }, [appliedAt, load]);

  useEffect(() => {
    if (!auto) return;
    const id = setInterval(() => document.visibilityState === 'visible' && void load(), 30000);
    return () => clearInterval(id);
  }, [auto, load]);

  return (
    <div className="live">
      <div className="row nowrap">
        {!compact && <h3 className="grow">{t('preview.title')}</h3>}
        <span className={compact ? 'grow dim' : 'dim'}>{at ? t('preview.updated', { time: at.toLocaleTimeString() }) : ''}</span>
        <Button size="sm" loading={busy} onClick={() => void load()} title={t('common.refresh')}>
          {!busy && <Icon name="refresh" size={15} />} {t('common.refresh')}
        </Button>
      </div>
      {countdown && (
        <div className="col" style={{ gap: 4 }}>
          <div className="dim">{t('preview.after_apply', { s: countdown.left })}</div>
          <div className="live-countdown">
            <i style={{ width: `${((countdown.total - countdown.left) / countdown.total) * 100}%` }} />
          </div>
        </div>
      )}
      <div className="live-frame">
        {url ? <img src={url} width={552} height={768} alt={t('preview.title')} /> : <div className="live-ph">{err ? <span>{err}</span> : <span className="spin" />}</div>}
        {url && err && (
          <span className="live-badge">
            <Badge tone="warn">{t('preview.stale')}</Badge>
          </span>
        )}
      </div>
      {url && err && <div className="dim">{err}</div>}
      <Switch checked={auto} onChange={setAuto} label={t('preview.auto')} />
      <div className="dim">{t('preview.hint')}</div>
    </div>
  );
}
