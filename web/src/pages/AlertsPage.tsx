import { useCallback, useEffect, useState } from 'react';
import * as api from '../api';
import { useEnumLabel, useI18n, type DictKey } from '../i18n';
import { useToast } from '../state/toast';
import { errorText } from '../state/errors';
import { Badge, Button, Card, EmptyState, Icon, PageHeader, Skeleton } from '../ui';

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
  useEffect(load, [load]);  // once per visit; the Refresh button re-fetches

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
      <PageHeader
        icon="bell"
        title={t('alerts.title')}
        subtitle={t('alerts.subtitle')}
        actions={
          <>
            <Button onClick={load}>
              <Icon name="refresh" size={16} /> {t('common.refresh')}
            </Button>
            <Button variant="primary" disabled={!list.some((a) => a.state === 'active')} onClick={() => void act(api.ackAllAlerts)}>
              <Icon name="check" size={16} /> {t('alerts.ack_all')}
            </Button>
          </>
        }
      />
      {err && <div className="notice warn">{err}</div>}
      {data && <div className="muted">{t('alerts.summary', { n: live.length, c: count('critical'), w: count('warning'), i: count('info') })}</div>}
      <Card>
        {!data && !err ? (
          <div className="card-body col">
            <Skeleton h={18} />
            <Skeleton h={18} w="80%" />
            <Skeleton h={18} w="60%" />
          </div>
        ) : !list.length ? (
          <EmptyState icon="ok" title={t('alerts.empty')}>{t('alerts.empty_desc')}</EmptyState>
        ) : (
          <div className="scroll-x">
            <table className="table stack-sm">
              <thead>
                <tr>
                  <th>{t('alerts.col.severity')}</th>
                  <th>{t('alerts.col.alert')}</th>
                  <th>{t('alerts.col.state')}</th>
                  <th>{t('alerts.col.last_seen')}</th>
                  <th />
                </tr>
              </thead>
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
                    <td className="cell-actions">
                      {a.state === 'active' && (
                        <Button size="sm" onClick={() => void act(() => api.ackAlert(a.id))}>
                          {t('alerts.ack')}
                        </Button>
                      )}
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
