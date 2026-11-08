import { useCallback, useEffect, useRef, useState, type FormEvent } from 'react';
import * as api from '../api';
import { useI18n, type DictKey, type TFn } from '../i18n';
import { signalQuality } from '../model/wifi';
import { useSession } from '../state/session';
import { useToast } from '../state/toast';
import { errorText, fieldMap } from '../state/errors';
import { Badge, Button, Card, Icon, Modal, SecretInput, TextInput, PageHeader, useConfirm } from '../ui';

function uptime(s: number, t: TFn): string {
  const d = Math.floor(s / 86400);
  const h = Math.floor((s % 86400) / 3600);
  const m = Math.floor((s % 3600) / 60);
  return d ? t('sys.uptime_dhm', { d, h, m }) : t('sys.uptime_hm', { h, m });
}
const kb = (n: number) => `${Math.round(n / 1024)} KiB`;
const MODES = ['online', 'init_provision', 'ap_fallback'];
const LEVELS = ['error', 'warn', 'info', 'debug'];

const REBOOT_WAIT_MS = 60000;

export function SystemPage() {
  const { t } = useI18n();
  const toast = useToast();
  const { status, connection, refreshStatus, logout } = useSession();
  const [logs, setLogs] = useState<api.LogEntry[]>([]);
  const [resetOpen, setResetOpen] = useState(false);
  const [confirmText, setConfirmText] = useState('');
  const [password, setPassword] = useState('');
  const [newKey, setNewKey] = useState('');
  const [pw, setPw] = useState({ old: '', next: '' });
  const [pwErr, setPwErr] = useState<Record<string, string>>({});
  const [busy, setBusy] = useState<'reboot' | 'reset' | 'rotate' | 'password' | null>(null);
  const [rebootAt, setRebootAt] = useState(0);
  const sawOffline = useRef(false);
  const [confirm, confirmDialog] = useConfirm();

  const loadLogs = useCallback(() => {
    api.getLogs(200).then((r) => setLogs(r.entries), (e) => toast('error', errorText(e, t)));
  }, [toast, t]);
  useEffect(() => {
    void refreshStatus();
    loadLogs();
  }, [refreshStatus, loadLogs]);

  // After a reboot, poll until the device answers again (or give up after a minute).
  useEffect(() => {
    if (!rebootAt) return;
    const id = setInterval(() => {
      if (Date.now() - rebootAt > REBOOT_WAIT_MS) {
        setRebootAt(0);
        toast('warn', t('sys.reboot_timeout'));
        return;
      }
      void refreshStatus();
    }, 3000);
    return () => clearInterval(id);
  }, [rebootAt, refreshStatus, toast, t]);
  useEffect(() => {
    if (!rebootAt) return;
    if (connection === 'offline') sawOffline.current = true;
    else if (connection === 'online' && sawOffline.current) {
      setRebootAt(0);
      toast('ok', t('sys.reboot_back'));
      loadLogs();
    }
  }, [connection, rebootAt, toast, t, loadLogs]);

  const closeReset = () => {
    if (busy) return;
    setResetOpen(false);
    setConfirmText('');
    setPassword('');
  };
  const doReboot = async () => {
    if (!(await confirm({ title: t('sys.reboot'), body: t('sys.reboot_h'), ok: t('sys.reboot') }))) return;
    setBusy('reboot');
    try {
      await api.reboot();
      sawOffline.current = false;
      setRebootAt(Date.now());
    } catch (e) {
      toast('error', errorText(e, t));
    } finally {
      setBusy(null);
    }
  };
  const doReset = async (e?: FormEvent) => {
    e?.preventDefault();
    if (confirmText !== 'RESET' || !password || busy) return;
    setBusy('reset');
    try {
      await api.factoryReset(password);
      toast('ok', t('sys.reset_done'));
      setResetOpen(false);
      setTimeout(() => void logout(), 1500);
    } catch (x) {
      toast('error', errorText(x, t));
    } finally {
      setBusy(null);
    }
  };
  const doRotate = async () => {
    if (!(await confirm({ title: t('sys.rotate_btn'), body: t('sys.rotate_h'), ok: t('sys.rotate_btn'), danger: true }))) return;
    setBusy('rotate');
    try {
      setNewKey((await api.rotateIngestKey()).ingest_key);
      void refreshStatus();
    } catch (e) {
      toast('error', errorText(e, t));
    } finally {
      setBusy(null);
    }
  };
  const copyKey = async () => {
    try {
      await navigator.clipboard.writeText(newKey);
      toast('ok', t('common.copied'));
    } catch {
      /* clipboard blocked: the key stays selectable */
    }
  };
  const doPassword = async (e: FormEvent) => {
    e.preventDefault();
    if (pw.next.length < 8) {
      setPwErr({ new_password: t('sys.password_short') });
      return;
    }
    setPwErr({});
    setBusy('password');
    try {
      await api.changePassword(pw.old, pw.next);
      setPw({ old: '', next: '' });
      // The device revokes every session, including this one.
      toast('ok', t('sys.password_changed'));
      await logout();
    } catch (x) {
      setPwErr(fieldMap(x));
      toast('error', errorText(x, t));
    } finally {
      setBusy(null);
    }
  };

  const mode = status?.device.mode ?? '';
  const ok = (v: boolean) => <Badge tone={v ? 'ok' : 'danger'}>{v ? t('sys.ok') : t('sys.bad')}</Badge>;

  return (
    <div className="page stack">
      <PageHeader icon="cpu" title={t('sys.title')} />
      {rebootAt > 0 && (
        <div className="notice warn" role="status">
          <span className="spin" aria-hidden />
          <span>{t('sys.rebooting')}</span>
        </div>
      )}
      <Card title={t('sys.status')} actions={<Button size="sm" onClick={() => void refreshStatus()}>{t('common.refresh')}</Button>}>
        {status ? (
          <table className="table">
            <tbody>
              <tr><th>{t('sys.fw')}</th><td className="mono">{status.device.fw_version} <span className="dim">{status.device.fw_sha256.slice(0, 12)}</span></td></tr>
              <tr><th>{t('sys.uptime')}</th><td>{uptime(status.device.uptime_s, t)}</td></tr>
              <tr><th>{t('sys.mode')}</th><td><Badge tone={mode === 'online' ? 'ok' : 'warn'}>{MODES.includes(mode) ? t(`sys.mode.${mode}` as DictKey) : mode}</Badge></td></tr>
              <tr><th>{t('sys.network')}</th><td>{status.network.ssid || '—'} · <span className="mono">{status.network.ip}</span> · {t(`wifi.signal.${signalQuality(status.network.rssi)}` as DictKey)} <span className="dim">{status.network.rssi} dBm</span></td></tr>
              <tr><th>{t('sys.time')}</th><td>{status.time.valid ? new Date(status.time.now * 1000).toLocaleString() : t('sys.time_unsynced')}</td></tr>
              <tr><th>{t('sys.memory')}</th><td>{kb(status.memory.internal_free)} / {kb(status.memory.psram_free)}</td></tr>
              <tr><th>{t('sys.storage')}</th><td className="row">{t('set.cap.sd')} {ok(status.storage.sd_ready)} {t('sys.config')} {ok(status.storage.config_valid)}</td></tr>
            </tbody>
          </table>
        ) : (
          <span className="dim">{t('common.loading')}</span>
        )}
      </Card>

      <Card title={t('sys.logs')} actions={<Button size="sm" onClick={loadLogs}>{t('common.refresh')}</Button>}>
        <div className="logs">
          {logs.map((l) => (
            <div key={l.seq}>
              <span className="dim">{l.unsynced ? `#${l.seq}` : new Date(l.ts * 1000).toLocaleTimeString()}</span>{' '}
              <Badge tone={l.level === 'error' ? 'danger' : l.level === 'warn' ? 'warn' : undefined}>{LEVELS.includes(l.level) ? t(`sys.level.${l.level}` as DictKey) : l.level}</Badge> {l.msg}
            </div>
          ))}
        </div>
      </Card>

      <Card title={t('sys.password')}>
        <form className="form-grid two" onSubmit={(e) => void doPassword(e)}>
          <input type="text" name="username" autoComplete="username" value="admin" readOnly hidden />
          <SecretInput label={t('sys.old_password')} autoComplete="current-password" value={pw.old} onChange={(v) => setPw({ ...pw, old: v })} error={pwErr.old_password} showLabel={t('auth.show')} hideLabel={t('auth.hide')} />
          <SecretInput label={t('sys.new_password')} autoComplete="new-password" value={pw.next} onChange={(v) => setPw({ ...pw, next: v })} error={pwErr.new_password} placeholder={t('sys.password_short')} maxLength={128} showLabel={t('auth.show')} hideLabel={t('auth.hide')} />
          <div className="wide">
            <Button type="submit" variant="primary" loading={busy === 'password'} disabled={!pw.old || !pw.next}>
              {t('sys.password_save')}
            </Button>
          </div>
        </form>
      </Card>

      <Card title={t('sys.maintenance')}>
        <div className="action-list">
          <div className="action-item">
            <div className="grow">
              <strong>{t('sys.rotate')}</strong>
              {status && <p className="mono dim">{status.security.ingest_key_masked}</p>}
            </div>
            <Button loading={busy === 'rotate'} onClick={() => void doRotate()}>
              <Icon name="refresh" size={15} /> {t('sys.rotate_btn')}
            </Button>
          </div>
          {newKey && (
            <div className="notice warn col">
              <span>{t('sys.rotate_done')}</span>
              <div className="row">
                <pre className="secret-reveal grow">{newKey}</pre>
                <Button size="sm" onClick={() => void copyKey()}>
                  {t('common.copy')}
                </Button>
              </div>
            </div>
          )}
          <div className="action-item">
            <div className="grow">
              <strong>{t('sys.reboot')}</strong>
              <p className="dim">{t('sys.reboot_h')}</p>
            </div>
            <Button loading={busy === 'reboot' || rebootAt > 0} onClick={() => void doReboot()}>
              {t('sys.reboot')}
            </Button>
          </div>
          <div className="action-item">
            <div className="grow">
              <strong>{t('sys.reset')}</strong>
              <p className="dim">{t('sys.reset_h')}</p>
            </div>
            <Button variant="danger" onClick={() => setResetOpen(true)}>
              {t('sys.reset')}
            </Button>
          </div>
        </div>
      </Card>

      {resetOpen && (
        <Modal
          title={t('sys.reset')}
          onClose={closeReset}
          footer={
            <>
              <Button onClick={closeReset} disabled={busy === 'reset'}>
                {t('common.cancel')}
              </Button>
              <Button type="submit" form="reset-form" variant="danger" className="solid" loading={busy === 'reset'} disabled={confirmText !== 'RESET' || !password}>
                {t('sys.reset')}
              </Button>
            </>
          }
        >
          <form id="reset-form" className="col" onSubmit={(e) => void doReset(e)}>
            <p>{t('sys.reset_confirm')}</p>
            <TextInput label="RESET" value={confirmText} onChange={setConfirmText} autoFocus autoComplete="off" />
            <SecretInput label={t('login.password')} autoComplete="current-password" value={password} onChange={setPassword} showLabel={t('auth.show')} hideLabel={t('auth.hide')} />
          </form>
        </Modal>
      )}
      {confirmDialog}
    </div>
  );
}
