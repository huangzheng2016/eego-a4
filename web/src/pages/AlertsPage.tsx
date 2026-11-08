import { useCallback, useEffect, useState } from 'react';
import { useReconnect } from '../state/session';
import * as api from '../api';
import { useI18n, type DictKey } from '../i18n';
import { useToast } from '../state/toast';
import { errorText } from '../state/errors';
import { Button, Card, EmptyState, Icon, PageHeader, Skeleton } from '../ui';
import { RelTime, SeverityBadge } from '../ui/badges';

const CLAMP2 = { display: '-webkit-box', WebkitLineClamp: 2, WebkitBoxOrient: 'vertical', overflow: 'hidden' } as const;

export function AlertsPage() {
  const { t } = useI18n();
  const toast = useToast();
  const [data, setData] = useState<{ alerts: api.AlertRecord[] } | null>(null);
  const [err, setErr] = useState<string | null>(null);
  const [loading, setLoading] = useState(false);
  const [busy, setBusy] = useState<string | null>(null);

  const load = useCallback(() => {
    setLoading(true);
    api.listAlerts(200)
      .then(
        (r) => {
          setData(r);
          setErr(null);
        },
        (e) => setErr(e instanceof api.ApiError && e.status === 503 ? t('alerts.unavailable') : errorText(e, t)),
      )
      .finally(() => setLoading(false));
  }, [t]);
  useReconnect(() => {
    if (err) void load();
  });
  useEffect(load, [load]);  // once per visit; the Refresh button re-fetches

  const act = async (id: string, fn: () => Promise<unknown>) => {
    if (busy) return;
    setBusy(id);
    try {
      await fn();
      load();
    } catch (e) {
      toast('error', errorText(e, t));
    } finally {
      setBusy(null);
    }
  };

  const list = data?.alerts ?? [];
  const live = list.filter((a) => a.state === 'active' || a.state === 'acked');
  const hasActive = list.some((a) => a.state === 'active');
  const summary = live.length
    ? [t('alerts.summary', { n: live.length }), ...(['critical', 'warning', 'info'] as const)
        .map((s) => [s, live.filter((a) => a.severity === s).length] as const)
        .filter(([, n]) => n > 0)
        .map(([s, n]) => `${t(`severity.${s}` as DictKey)} ${n}`)].join(' · ')
    : null;
  return (
    <div className="page stack">
      <PageHeader
        icon="bell"
        title={t('alerts.title')}
        subtitle={t('alerts.subtitle')}
        actions={
          <>
            <Button onClick={load} loading={loading}>
              <Icon name="refresh" size={16} /> {t('common.refresh')}
            </Button>
            {hasActive && (
              <Button variant="primary" loading={busy === '*'} onClick={() => void act('*', api.ackAllAlerts)}>
                <Icon name="check" size={16} /> {t('alerts.ack_all')}
              </Button>
            )}
          </>
        }
      />
      {summary && <div className="muted">{summary}</div>}
      <Card>
        {err && !data ? (
          <div className="card-body"><div className="notice warn">{err}</div></div>
        ) : !data ? (
          <div className="card-body col">
            <Skeleton h={18} />
            <Skeleton h={18} w="80%" />
            <Skeleton h={18} w="60%" />
          </div>
        ) : (
          <>
            {err && <div className="card-body"><div className="notice warn">{err}</div></div>}
            {!list.length ? (
              <EmptyState icon="ok" title={t('alerts.empty')} />
            ) : (
              <div className="scroll-x" style={{ maxHeight: '70vh', overflowY: 'auto' }}>
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
                          <SeverityBadge sev={a.severity} />
                        </td>
                        <td title={a.type}>
                          <strong>{a.title}</strong>
                          {a.message && <div className="dim" style={CLAMP2} title={a.message}>{a.message}</div>}
                        </td>
                        <td>
                          {t(`alerts.state.${a.state}` as DictKey)}
                          {(a.occurrences ?? 1) > 1 && <div className="dim">{t('alerts.occurrences', { n: a.occurrences ?? 1 })}</div>}
                        </td>
                        <td className="dim" style={{ whiteSpace: 'nowrap' }}><RelTime ts={a.last_seen} /></td>
                        <td className="cell-actions">
                          {a.state === 'active' && (
                            <Button size="sm" loading={busy === a.id + ':ack'} disabled={!!busy} onClick={() => void act(a.id + ':ack', () => api.ackAlert(a.id))}>
                              {t('alerts.ack')}
                            </Button>
                          )}
                          {(a.state === 'active' || a.state === 'acked') && (
                            <Button
                              size="sm"
                              variant="ghost"
                              loading={busy === a.id + ':clear'}
                              disabled={!!busy}
                              onClick={() => {
                                if (confirm(t('alerts.clear_confirm', { title: a.title }))) void act(a.id + ':clear', () => api.clearAlert(a.id));
                              }}
                            >
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
          </>
        )}
      </Card>
    </div>
  );
}
