import { useCallback, useEffect, useState } from 'react';
import * as api from '../api';
import { useI18n } from '../i18n';
import { useSession } from '../state/session';
import { useToast } from '../state/toast';
import { errorText, fieldMap } from '../state/errors';
import { Badge, Button, Card, Modal, TextInput, PageHeader } from '../ui';

function uptime(s: number): string {
  const d = Math.floor(s / 86400);
  const h = Math.floor((s % 86400) / 3600);
  const m = Math.floor((s % 3600) / 60);
  return `${d ? `${d}d ` : ''}${h}h ${m}m`;
}
const kb = (n: number) => `${Math.round(n / 1024)} KiB`;

export function SystemPage() {
  const { t } = useI18n();
  const toast = useToast();
  const { status, refreshStatus, logout } = useSession();
  const [logs, setLogs] = useState<api.LogEntry[]>([]);
  const [dialog, setDialog] = useState<'reboot' | 'reset' | null>(null);
  const [confirmText, setConfirmText] = useState('');
  const [password, setPassword] = useState('');
  const [newKey, setNewKey] = useState('');
  const [pw, setPw] = useState({ old: '', next: '' });
  const [pwErr, setPwErr] = useState<Record<string, string>>({});

  const loadLogs = useCallback(() => {
    api.getLogs(200).then((r) => setLogs(r.entries), (e) => toast('error', errorText(e, t)));
  }, [toast, t]);
  useEffect(() => {
    void refreshStatus();
    loadLogs();
  }, [refreshStatus, loadLogs]);

  const closeDialog = () => {
    setDialog(null);
    setConfirmText('');
    setPassword('');
  };
  const doReboot = async () => {
    try {
      await api.reboot();
      toast('ok', t('sys.rebooting'));
      closeDialog();
    } catch (e) {
      toast('error', errorText(e, t));
    }
  };
  const doReset = async () => {
    try {
      await api.factoryReset(password);
      toast('ok', t('sys.reset_done'));
      closeDialog();
      setTimeout(() => void logout(), 1500);
    } catch (e) {
      toast('error', errorText(e, t));
    }
  };
  const doRotate = async () => {
    try {
      setNewKey((await api.rotateIngestKey()).ingest_key);
    } catch (e) {
      toast('error', errorText(e, t));
    }
  };
  const doPassword = async () => {
    setPwErr({});
    try {
      await api.changePassword(pw.old, pw.next);
      setPw({ old: '', next: '' });
      toast('ok', t('sys.password_changed'));
    } catch (e) {
      setPwErr(fieldMap(e));
      toast('error', errorText(e, t));
    }
  };

  return (
    <div className="page stack">
      <PageHeader icon="cpu" title={t('sys.title')} subtitle={t('sys.subtitle')} />
      <Card title={t('sys.status')} actions={<Button size="sm" onClick={() => void refreshStatus()}>{t('common.refresh')}</Button>}>
        {status ? (
          <table className="table">
            <tbody>
              <tr><th>{t('sys.fw')}</th><td className="mono">{status.device.fw_version} <span className="dim">{status.device.fw_sha256.slice(0, 12)}</span></td></tr>
              <tr><th>{t('sys.uptime')}</th><td>{uptime(status.device.uptime_s)}</td></tr>
              <tr><th>{t('sys.mode')}</th><td><Badge tone={status.device.mode === 'online' ? 'ok' : 'warn'}>{status.device.mode}</Badge></td></tr>
              <tr><th>{t('sys.network')}</th><td>{status.network.ssid || '—'} · <span className="mono">{status.network.ip}</span> · {status.network.rssi} dBm</td></tr>
              <tr><th>{t('sys.time')}</th><td>{status.time.valid ? new Date(status.time.now * 1000).toLocaleString() : t('sys.time_unsynced')}</td></tr>
              <tr><th>{t('sys.memory')}</th><td>{kb(status.memory.internal_free)} / {kb(status.memory.psram_free)}</td></tr>
              <tr><th>{t('sys.storage')}</th><td>SD {status.storage.sd_ready ? '✓' : '✗'} · config {status.storage.config_valid ? '✓' : '✗'}</td></tr>
              <tr><th>{t('sys.ingest_key')}</th><td className="mono">{status.security.ingest_key_masked}</td></tr>
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
              <Badge tone={l.level === 'error' ? 'danger' : l.level === 'warn' ? 'warn' : undefined}>{l.level}</Badge> {l.msg}
            </div>
          ))}
        </div>
      </Card>

      <Card title={t('sys.password')}>
        <div className="form-grid two">
          <TextInput label={t('sys.old_password')} type="password" autoComplete="current-password" value={pw.old} onChange={(v) => setPw({ ...pw, old: v })} error={pwErr.old_password} />
          <TextInput label={t('sys.new_password')} type="password" autoComplete="new-password" value={pw.next} onChange={(v) => setPw({ ...pw, next: v })} error={pwErr.new_password} />
          <div className="wide">
            <Button disabled={!pw.old || !pw.next} onClick={() => void doPassword()}>
              {t('common.save')}
            </Button>
          </div>
        </div>
      </Card>

      <Card title={t('sys.danger')}>
        <div className="row">
          <Button onClick={() => void doRotate()}>{t('sys.rotate')}</Button>
          <Button variant="danger" onClick={() => setDialog('reboot')}>
            {t('sys.reboot')}
          </Button>
          <Button variant="danger" onClick={() => setDialog('reset')}>
            {t('sys.reset')}
          </Button>
        </div>
        {newKey && (
          <div className="notice warn" style={{ marginTop: 10 }}>
            {t('sys.rotate_done')}
            <pre style={{ marginTop: 6 }}>{newKey}</pre>
          </div>
        )}
      </Card>

      {dialog && (
        <Modal
          title={dialog === 'reboot' ? t('sys.reboot') : t('sys.reset')}
          onClose={closeDialog}
          footer={
            <>
              <Button onClick={closeDialog}>{t('common.cancel')}</Button>
              <Button
                variant="danger"
                className="solid"
                disabled={dialog === 'reboot' ? confirmText !== 'REBOOT' : confirmText !== 'RESET' || !password}
                onClick={() => void (dialog === 'reboot' ? doReboot() : doReset())}
              >
                {t('common.confirm')}
              </Button>
            </>
          }
        >
          <div className="col">
            <p>{dialog === 'reboot' ? t('sys.reboot_confirm') : t('sys.reset_confirm')}</p>
            <TextInput label={dialog === 'reboot' ? 'REBOOT' : 'RESET'} value={confirmText} onChange={setConfirmText} autoFocus />
            {dialog === 'reset' && <TextInput label={t('login.password')} type="password" value={password} onChange={setPassword} />}
          </div>
        </Modal>
      )}
    </div>
  );
}
