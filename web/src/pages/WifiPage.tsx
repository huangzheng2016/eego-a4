import { useCallback, useEffect, useState } from 'react';
import * as api from '../api';
import { useI18n, type DictKey } from '../i18n';
import { useToast } from '../state/toast';
import { errorText } from '../state/errors';
import { Badge, Button, Card, Checkbox, Empty, Icon, Modal, PageHeader, Spinner, TextInput } from '../ui';
import { disconnectReasonText, moveItem, rowsFromNetworks, signalBars, signalQuality, toEdits, validateRow, type WifiRow } from '../model/wifi';

interface EditState {
  /** Row index being edited, -1 for a new network. */
  index: number;
  ssid: string;
  password: string;
  open: boolean;
  errors: Partial<Record<'ssid' | 'password', string>>;
}

const STATES = ['connected', 'connecting', 'disconnected', 'failed', 'offline'];

export function WifiPage() {
  const { t } = useI18n();
  const toast = useToast();
  const [data, setData] = useState<api.WifiList | null>(null);
  const [rows, setRows] = useState<WifiRow[]>([]);
  const [busy, setBusy] = useState(false);
  const [edit, setEdit] = useState<EditState | null>(null);
  const [remove, setRemove] = useState<number | null>(null);
  const [drag, setDrag] = useState<number | null>(null);

  const apply = useCallback((r: api.WifiList) => {
    setData(r);
    setRows(rowsFromNetworks(r.networks));
  }, []);
  const load = useCallback(() => {
    api.getWifi().then(apply, (e) => toast('error', errorText(e, t)));
  }, [apply, toast, t]);
  useEffect(load, [load]);

  const save = async (next: WifiRow[], okKey: DictKey, vars?: Record<string, string>) => {
    setBusy(true);
    try {
      apply(await api.putWifi(toEdits(next)));
      toast('ok', t(okKey, vars));
      return true;
    } catch (e) {
      toast('error', errorText(e, t));
      load();
      return false;
    } finally {
      setBusy(false);
    }
  };

  const current = data?.current;
  const inUse = (i: number) => !!current?.connected && current.index === i;
  const max = data?.max ?? 8;

  const move = (from: number, to: number) => {
    if (from === to || to < 0 || to >= rows.length) return;
    const next = moveItem(rows, from, to);
    setRows(next);
    void save(next, 'wifi.reordered');
  };

  const openAdd = () => setEdit({ index: -1, ssid: '', password: '', open: false, errors: {} });
  const openEdit = (i: number) => setEdit({ index: i, ssid: rows[i].ssid, password: '', open: !rows[i].hasPassword, errors: {} });

  const submitEdit = async () => {
    if (!edit) return;
    const base = edit.index >= 0 ? rows[edit.index] : undefined;
    const row: WifiRow = { ssid: edit.ssid, hasPassword: !edit.open };
    if (edit.open) row.password = '';
    else if (edit.password) row.password = edit.password;
    // else: undefined keeps the stored password (validateRow rejects it for new rows)
    if (base) row.previousSsid = base.ssid;
    if (base && !edit.open && !edit.password && !base.hasPassword) {
      setEdit({ ...edit, errors: { password: t('wifi.err.password_required') } });
      return;
    }
    const errs = validateRow(row, rows, edit.index);
    if (errs.ssid || errs.password) {
      setEdit({
        ...edit,
        errors: {
          ssid: errs.ssid && t(`wifi.err.${errs.ssid}` as DictKey),
          password: errs.password && t(`wifi.err.${errs.password}` as DictKey),
        },
      });
      return;
    }
    const next = edit.index >= 0 ? rows.map((r, i) => (i === edit.index ? row : r)) : [...rows, row];
    if (await save(next, 'wifi.saved')) setEdit(null);
  };

  const confirmRemove = async () => {
    if (remove === null) return;
    const ssid = rows[remove].ssid;
    if (await save(rows.filter((_, i) => i !== remove), 'wifi.deleted', { ssid })) setRemove(null);
  };

  const stateLabel = (s: string) => (STATES.includes(s) ? t(`wifi.state.${s}` as DictKey) : s);

  return (
    <div className="page stack">
      <PageHeader icon="wifi" title={t('wifi.title')} subtitle={t('wifi.subtitle')} />
      <div className="notice">{t('wifi.note')}</div>

      <Card title={t('wifi.status')} actions={<Button size="sm" onClick={load}>{t('common.refresh')}</Button>}>
        {!current ? (
          <Spinner />
        ) : (
          <table className="table">
            <tbody>
              <tr>
                <th>{t('wifi.state')}</th>
                <td>
                  <Badge tone={current.connected ? 'ok' : current.state === 'connecting' ? 'accent' : 'warn'} dot>
                    {stateLabel(current.state)}
                  </Badge>
                </td>
              </tr>
              {current.connected && (
                <>
                  <tr>
                    <th>{t('wifi.ssid')}</th>
                    <td>
                      {current.ssid}
                      {current.index >= 0 && <span className="dim"> · #{current.index + 1}</span>}
                    </td>
                  </tr>
                  <tr>
                    <th>{t('wifi.signal')}</th>
                    <td>
                      <SignalBars bars={signalBars(signalQuality(current.rssi))} /> {current.rssi} dBm{' '}
                      <span className={signalQuality(current.rssi) === 'weak' ? 'warn-text' : 'dim'}>· {t(`wifi.signal.${signalQuality(current.rssi)}` as DictKey)}</span>
                    </td>
                  </tr>
                  {signalQuality(current.rssi) === 'weak' && (
                    <tr>
                      <th />
                      <td className="warn-text">{t('wifi.signal_hint')}</td>
                    </tr>
                  )}
                  {current.rssi_min_10m !== undefined && current.rssi_min_10m !== 0 && (
                    <tr>
                      <th>{t('wifi.link')}</th>
                      <td className="dim">
                        {t('wifi.link_value', {
                          min: String(current.rssi_min_10m),
                          avg: String(current.rssi_avg_10m ?? 0),
                          bt: String(current.beacon_timeouts ?? 0),
                          roams: String(current.roams ?? 0),
                        })}
                      </td>
                    </tr>
                  )}
                  {current.last_dhcp_ms !== undefined && current.last_dhcp_ms >= 0 && (
                    <tr>
                      <th>{t('wifi.dhcp')}</th>
                      <td className="mono">{(current.last_dhcp_ms / 1000).toFixed(1)} s</td>
                    </tr>
                  )}
                  <tr>
                    <th>{t('wifi.ip')}</th>
                    <td className="mono">{current.ip || '—'}</td>
                  </tr>
                </>
              )}
              {!current.connected && !!current.offline_ms && (
                <tr>
                  <th />
                  <td className="dim">
                    {t('wifi.offline_for', { s: Math.round(current.offline_ms / 1000), b: Math.round((current.reconnect_backoff_ms ?? 0) / 1000) })}
                  </td>
                </tr>
              )}
              <tr>
                <th>{t('wifi.last_reason')}</th>
                <td className="mono">{disconnectReasonText(current.last_disconnect_reason ?? 0)}</td>
              </tr>
              <tr>
                <th>{t('wifi.reconnects')}</th>
                <td className="mono">
                  {current.reconnect_attempts ?? 0} / {current.disconnects ?? 0}
                </td>
              </tr>
            </tbody>
          </table>
        )}
      </Card>

      <Card
        title={t('wifi.list', { n: rows.length, max })}
        actions={
          <Button size="sm" variant="primary" onClick={openAdd} disabled={!data || busy || rows.length >= max} title={rows.length >= max ? t('wifi.full', { max }) : undefined}>
            <Icon name="plus" size={14} /> {t('wifi.add')}
          </Button>
        }
      >
        {!data ? (
          <Spinner />
        ) : !rows.length ? (
          <Empty>{t('wifi.empty')}</Empty>
        ) : (
          <>
            <p className="dim card-lead">{t('wifi.list_hint')}</p>
            <table className="table stack-sm">
              <thead>
                <tr>
                  <th className="col-narrow">{t('wifi.priority')}</th>
                  <th>SSID</th>
                  <th>{t('wifi.security')}</th>
                  <th />
                </tr>
              </thead>
              <tbody>
                {rows.map((r, i) => (
                  <tr
                    key={r.ssid}
                    draggable={!busy}
                    title={t('wifi.drag')}
                    onDragStart={(e) => {
                      setDrag(i);
                      e.dataTransfer.effectAllowed = 'move';
                    }}
                    onDragOver={(e) => drag !== null && e.preventDefault()}
                    onDrop={(e) => {
                      e.preventDefault();
                      if (drag !== null) move(drag, i);
                      setDrag(null);
                    }}
                    onDragEnd={() => setDrag(null)}
                    className={[drag === i ? 'faded' : '', busy ? '' : 'draggable'].filter(Boolean).join(' ') || undefined}
                  >
                    <td className="mono">
                      <Icon name="move" size={14} /> {i + 1}
                    </td>
                    <td>
                      {r.ssid} {inUse(i) && <Badge tone="ok">{t('wifi.in_use')}</Badge>}
                    </td>
                    <td className="dim">{r.hasPassword ? t('wifi.secured') : t('wifi.open')}</td>
                    <td className="cell-actions">
                      <Button size="sm" icon variant="ghost" aria-label={t('wifi.up')} title={t('wifi.up')} disabled={busy || i === 0} onClick={() => move(i, i - 1)}>
                        <Icon name="up" size={14} />
                      </Button>
                      <Button size="sm" icon variant="ghost" aria-label={t('wifi.down')} title={t('wifi.down')} disabled={busy || i === rows.length - 1} onClick={() => move(i, i + 1)}>
                        <Icon name="down" size={14} />
                      </Button>{' '}
                      <Button size="sm" disabled={busy} onClick={() => openEdit(i)}>
                        {t('common.edit')}
                      </Button>{' '}
                      <Button size="sm" variant="danger" disabled={busy} onClick={() => setRemove(i)}>
                        {t('common.delete')}
                      </Button>
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </>
        )}
      </Card>

      {edit && (
        <Modal
          title={edit.index >= 0 ? t('wifi.edit') : t('wifi.add')}
          onClose={() => setEdit(null)}
          footer={
            <>
              <Button onClick={() => setEdit(null)}>{t('common.cancel')}</Button>
              <Button variant="primary" loading={busy} onClick={() => void submitEdit()}>
                {t('common.save')}
              </Button>
            </>
          }
        >
          <form
            className="col"
            autoComplete="off"
            onSubmit={(e) => {
              e.preventDefault();
              void submitEdit();
            }}
          >
            {edit.index >= 0 && inUse(edit.index) && (
              <div className="notice warn">
                <Icon name="warn" size={16} />
                <span>{t('wifi.edit_current')}</span>
              </div>
            )}
            <TextInput label={t('wifi.ssid')} value={edit.ssid} onChange={(v) => setEdit({ ...edit, ssid: v, errors: { ...edit.errors, ssid: undefined } })} error={edit.errors.ssid} maxLength={32} autoFocus required />
            <Checkbox label={t('wifi.open_check')} checked={edit.open} onChange={(v) => setEdit({ ...edit, open: v, errors: { ...edit.errors, password: undefined } })} />
            {!edit.open && (
              <TextInput
                label={t('wifi.password')}
                type="password"
                autoComplete="new-password"
                value={edit.password}
                onChange={(v) => setEdit({ ...edit, password: v, errors: { ...edit.errors, password: undefined } })}
                error={edit.errors.password}
                hint={edit.index >= 0 && rows[edit.index].hasPassword ? t('wifi.password_keep') : undefined}
                maxLength={63}
              />
            )}
            <button type="submit" hidden />
          </form>
        </Modal>
      )}

      {remove !== null && rows[remove] && (
        <Modal
          title={t('common.delete')}
          onClose={() => setRemove(null)}
          footer={
            <>
              <Button onClick={() => setRemove(null)}>{t('common.cancel')}</Button>
              <Button variant="danger" className="solid" loading={busy} onClick={() => void confirmRemove()}>
                {t('common.delete')}
              </Button>
            </>
          }
        >
          <div className="col">
            <p>{t('wifi.delete_confirm', { ssid: rows[remove].ssid })}</p>
            {inUse(remove) && (
              <div className="notice warn">
                <Icon name="warn" size={16} />
                <span>{t('wifi.delete_current')}</span>
              </div>
            )}
            {rows.length === 1 && (
              <div className="notice warn">
                <Icon name="warn" size={16} />
                <span>{t('wifi.delete_last')}</span>
              </div>
            )}
          </div>
        </Modal>
      )}
    </div>
  );
}

function SignalBars({ bars }: { bars: number }) {
  return (
    <span className="signal-bars" aria-hidden="true">
      {[1, 2, 3, 4].map((i) => (
        <i key={i} className={i <= bars ? 'on' : undefined} />
      ))}
    </span>
  );
}
