// App-shell power controls: the "keep awake" toggle (header/sidebar) and the
// "battery device will sleep soon" hint. Logic lives in model/power.ts.
import { useEffect, useState } from 'react';
import * as api from '../api';
import { useI18n } from '../i18n';
import { keepAwakeLeft, minutesLeft, probablyAsleep, sleepIn, sleepSoon, toggleMinutes } from '../model/power';
import { useSession } from '../state/session';
import { errorText } from '../state/errors';
import { useToast } from '../state/toast';
import { Button, Icon } from '../ui';

/** Re-render every `ms` while `on` (local countdowns between status polls). */
function useTick(on: boolean, ms = 1000): number {
  const [now, setNow] = useState(() => performance.now());
  useEffect(() => {
    if (!on) return;
    const id = setInterval(() => setNow(performance.now()), ms);
    return () => clearInterval(id);
  }, [on, ms]);
  return now;
}

function useKeepAwakeAction() {
  const { t } = useI18n();
  const toast = useToast();
  const { status, setKeepAwake, refreshStatus } = useSession();
  const [busy, setBusy] = useState(false);
  const run = async (minutes: number) => {
    setBusy(true);
    try {
      const ka = await api.setKeepAwake(minutes);
      setKeepAwake(ka);
      toast(minutes === 0 ? 'info' : 'ok', minutes === 0 ? t('ka.cancelled') : t('ka.started', { m: minutes }));
      void refreshStatus();
    } catch (e) {
      toast('error', errorText(e, t));
    } finally {
      setBusy(false);
    }
  };
  return { busy, run, ka: status?.keep_awake };
}

/** Prominent keep-awake toggle; shows the remaining minutes while active. */
export function KeepAwakeToggle({ compact = false }: { compact?: boolean }) {
  const { t } = useI18n();
  const { status, statusAt } = useSession();
  const { busy, run, ka } = useKeepAwakeAction();
  const now = useTick(!!ka?.active, 15000);
  if (!status || !ka) return null; // firmware without the keep-awake API
  const left = keepAwakeLeft(ka, statusAt, now);
  const active = left > 0;
  const target = toggleMinutes(active ? ka : { ...ka, active: false });
  const label = active ? t('ka.on', { m: minutesLeft(left) }) : t('ka.toggle');
  return (
    <Button
      size="sm"
      variant={active ? 'primary' : 'default'}
      loading={busy}
      aria-pressed={active}
      title={active ? t('ka.title_on') : t('ka.title_off', { m: target })}
      onClick={() => void run(target)}
      style={compact ? undefined : { justifyContent: 'flex-start' }}
    >
      <Icon name="sun" size={16} /> {compact && active ? `${minutesLeft(left)}′` : label}
    </Button>
  );
}

/** Battery device about to auto-sleep (and keep-awake off): explain and offer keep-awake. */
export function PowerBanner() {
  const { t } = useI18n();
  const { status, statusAt, connection } = useSession();
  const { busy, run, ka } = useKeepAwakeAction();
  const watching = !!status?.power && status.power.source === 'battery' && !ka?.active;
  const now = useTick(watching);
  const left = sleepIn(status?.power, !!ka?.active, statusAt, now);
  if (connection === 'offline' && probablyAsleep(status?.power)) {
    return (
      <div className="notice danger" role="status" style={{ margin: '12px 16px 0', alignItems: 'center' }}>
        <Icon name="moon" size={16} />
        <span className="grow">{t('err.asleep')}</span>
      </div>
    );
  }
  if (!sleepSoon(left)) return null;
  const minutes = toggleMinutes({ active: false, remaining_s: 0, until: 0, default_min: ka?.default_min });
  return (
    <div className="notice warn" role="status" style={{ margin: '12px 16px 0', alignItems: 'center' }}>
      <Icon name="moon" size={16} />
      <span className="grow">{t('power.sleep_soon', { s: left ?? 0 })}</span>
      <Button size="sm" variant="primary" loading={busy} onClick={() => void run(minutes)}>
        <Icon name="sun" size={15} /> {t('power.keep_awake_now', { m: minutes })}
      </Button>
    </div>
  );
}
