import { useCallback, useEffect, useState } from 'react';
import * as api from '../api';
import { useEnumLabel, useI18n, type DictKey } from '../i18n';
import { useToast } from '../state/toast';
import { errorText } from '../state/errors';
import { Badge, Button, Card, Empty, Spinner } from '../ui';

function fmt(ts?: number): string {
  if (!ts) return '—';
  return new Date(ts * 1000).toLocaleString();
}

export function AlertsPage() {
  const { t } = useI18n();
  const label = useEnumLabel();
  const toast = useToast();
  const [data, setData] = useState<{ alerts: api.AlertRecord[] } | null>(null);
  const [err, setErr] = useState<string | null>(null);

  const load = useCallback(() => {
    api.listAlerts(200).then(
      (r) => {
        setData(r);
        setErr(null);
      },
      (e) => setErr(e instanceof api.ApiError && e.status === 503 ? t('alerts.unavailable') : errorText(e, t)),
    );
  }, [t]);
  useEffect(() => {
    load();
    const id = setInterval(() => document.visibilityState === 'visible' && load(), 30000);
    return () => clearInterval(id);
  }, [load]);

  const act = async (fn: () => Promise<unknown>) => {
    try {
      await fn();
      load();
    } catch (e) {
      toast('error', errorText(e, t));
    }
  };

  const list = data?.alerts ?? [];
  const live = list.filter((a) => a.state === 'active' || a.state === 'acked');
  const count = (s: string) => live.filter((a) => a.severity === s).length;
  return (
    <div className="page stack">
      <div className="page-head">
        <h1 className="grow">{t('alerts.title')}</h1>
        <Button onClick={load}>{t('common.refresh')}</Button>
        <Button disabled={!list.some((a) => a.state === 'active')} onClick={() => void act(api.ackAllAlerts)}>
          {t('alerts.ack_all')}
        </Button>
      </div>
      {err && <div className="notice warn">{err}</div>}
      {data && <div className="muted">{t('alerts.summary', { n: live.length, c: count('critical'), w: count('warning'), i: count('info') })}</div>}
      <Card>
        {!data && !err ? (
          <Spinner />
        ) : !list.length ? (
          <Empty>{t('alerts.empty')}</Empty>
        ) : (
          <div className="scroll-x">
            <table className="table">
              <tbody>
                {list.map((a) => (
                  <tr key={a.id} className={a.state === 'cleared' || a.state === 'expired' ? 'faded' : ''}>
                    <td>
                      <Badge tone={a.severity === 'critical' ? 'danger' : a.severity === 'warning' ? 'warn' : undefined}>{label('severity', a.severity)}</Badge>
                    </td>
                    <td>
                      <strong>{a.title}</strong>
                      {a.message && <div className="dim">{a.message}</div>}
                      <div className="dim mono">{a.type}</div>
                    </td>
                    <td>
                      {t(`alerts.state.${a.state}` as DictKey)}
                      {(a.occurrences ?? 1) > 1 && <div className="dim">{t('alerts.occurrences', { n: a.occurrences ?? 1 })}</div>}
                    </td>
                    <td className="dim">{fmt(a.last_seen)}</td>
                    <td style={{ textAlign: 'right', whiteSpace: 'nowrap' }}>
                      {a.state === 'active' && (
                        <Button size="sm" onClick={() => void act(() => api.ackAlert(a.id))}>
                          {t('alerts.ack')}
                        </Button>
                      )}{' '}
                      {(a.state === 'active' || a.state === 'acked') && (
                        <Button size="sm" onClick={() => void act(() => api.clearAlert(a.id))}>
                          {t('alerts.clear')}
                        </Button>
                      )}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </Card>
    </div>
  );
}
