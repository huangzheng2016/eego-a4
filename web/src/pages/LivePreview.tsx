// frame. Fetched only on demand (Refresh), a few seconds after every successful
// apply (the e-ink refresh takes a moment) and, opt-in, every 30 s — opening the
// tab does not download a frame. With several
// screens, ‹ › switch the device's current screen (POST /device/screen).
import { useCallback, useEffect, useRef, useState } from 'react';
import * as api from '../api';
import { useI18n } from '../i18n';
import { useDraft } from '../state/draft';
import { errorText } from '../state/errors';
import { Badge, Button, Icon, Switch } from '../ui';
import { Pager } from '../ui/controls';

const AFTER_APPLY_S = [4, 12];

/** Auto-refresh preference kept across route changes. */
let autoPref = false;

export function LivePreview() {
  const { t } = useI18n();
  const { appliedAt } = useDraft();
  const [url, setUrl] = useState<string | null>(null);
  const [err, setErr] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const [auto, setAutoState] = useState(autoPref);
  const setAuto = (v: boolean) => {
    autoPref = v;
    setAutoState(v);
  };
  const [at, setAt] = useState<Date | null>(null);
  const [countdown, setCountdown] = useState<{ left: number; total: number } | null>(null);
  const urlRef = useRef<string | null>(null);
  const [screen, setScreen] = useState<api.DeviceScreen | null>(null);
  const [paging, setPaging] = useState(false);

  const load = useCallback(async () => {
    setBusy(true);
    api.getDeviceScreen().then(setScreen, () => setScreen(null));
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

  useEffect(
    () => () => {
      if (urlRef.current) URL.revokeObjectURL(urlRef.current);
    },
    [],
  );

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

  const page = async (step: number) => {
    setPaging(true);
    try {
      const r = await api.setDeviceScreen({ step });
      setScreen((s) => (s ? { ...s, index: r.index, count: r.count, shown: true } : s));
      setTimeout(() => void load(), 2500);  // e-ink refresh takes a moment
    } catch (e) {
      setErr(errorText(e, t));
    } finally {
      setPaging(false);
    }
  };

  useEffect(() => {
    if (!auto) return;
    const id = setInterval(() => document.visibilityState === 'visible' && void load(), 30000);
    return () => clearInterval(id);
  }, [auto, load]);

  return (
    <div className="live">
      <div className="row nowrap">
        <span className="grow dim">{at ? t('preview.updated', { time: at.toLocaleTimeString() }) : ''}</span>
        <Button size="sm" loading={busy} onClick={() => void load()}>
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
        {url ? (
          <img src={url} width={552} height={768} alt={t('preview.title')} />
        ) : (
          <div className="live-ph">
            {err ? (
              <span>{err}</span>
            ) : busy ? (
              <span className="spin" />
            ) : (
              <span className="dim">{t('preview.load')}</span>
            )}
          </div>
        )}
        {url && err && (
          <span className="live-badge" title={err}>
            <Badge tone="warn">{t('preview.stale')}</Badge>
          </span>
        )}
      </div>
      {screen?.available && screen.count > 1 && (
        <Pager
          label={t('editor.render_screen', { n: screen.index + 1, total: screen.count })}
          prevLabel={t('preview.device_prev')}
          nextLabel={t('preview.device_next')}
          prevDisabled={paging}
          nextDisabled={paging}
          onPrev={() => void page(-1)}
          onNext={() => void page(1)}
        />
      )}
      <Switch checked={auto} onChange={setAuto} label={t('preview.auto')} />
    </div>
  );
}
