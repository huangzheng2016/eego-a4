// Bluetooth entry on the login card, and first-time setup over BLE.
import { useState, type FormEvent, type ReactNode } from 'react';
import * as api from '../api';
import { useI18n } from '../i18n';
import { useSession } from '../state/session';
import { Button, Icon, Logo, Spinner, TextInput } from '../ui';
import { bleSupported, PAGES_URL } from '../model/origin';
import { isStalePairing } from '../api/ble/bleTransport';

/** `primary`: the only way in (Pages origin); otherwise a secondary option under the password form. */
export function BleEntry({ primary }: { primary: boolean }) {
  const { t } = useI18n();
  const { connectBle } = useSession();
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState('');
  if (!bleSupported()) {
    if (primary)
      return (
        <div className="notice warn">
          <Icon name="warn" size={16} />
          <span>{t('bt.unsupported')}</span>
        </div>
      );
    // The device origin is plain http, where browsers disable Web Bluetooth: open the HTTPS app.
    return (
      <a className="dim" href={`${PAGES_URL}?ble`}>
        {t('bt.use_ble')}
      </a>
    );
  }
  const go = async () => {
    setBusy(true);
    setErr('');
    try {
      await connectBle();
    } catch (e) {
      // NotFoundError: the user closed the device chooser.
      if (isStalePairing(e)) setErr(t('bt.stale_pairing'));
      else if ((e as Error).name !== 'NotFoundError') setErr(`${t('bt.failed')}（${(e as Error).message || (e as Error).name}）`);
    } finally {
      setBusy(false);
    }
  };
  return (
    <div className="col">
      <Button variant={primary ? 'primary' : 'ghost'} className={primary ? 'btn-lg' : undefined} loading={busy} onClick={() => void go()}>
        {t(primary ? 'bt.connect' : 'bt.use_ble')}
      </Button>
      {busy && <span className="dim">{t('bt.pairing')}</span>}
      {err && <span className="err-text">{err}</span>}
    </div>
  );
}

function Frame({ children }: { children: ReactNode }) {
  const { t } = useI18n();
  return (
    <div className="login">
      <div className="card login-card">
        <div className="brand">
          <Logo size={36} />
          <div>
            <div className="brand-name">{t('app.brand')}</div>
            <div className="brand-sub">{t('app.tagline')}</div>
          </div>
        </div>
        {children}
      </div>
    </div>
  );
}

/** Connected over BLE to a device without an admin password: set it (and optionally WiFi). */
export function BleSetup() {
  const { t } = useI18n();
  const { ble, disconnectBle } = useSession();
  const [pw, setPw] = useState('');
  const [pw2, setPw2] = useState('');
  const [ssid, setSsid] = useState('');
  const [wpw, setWpw] = useState('');
  const [tried, setTried] = useState(false);
  const [busy, setBusy] = useState(false);
  const [done, setDone] = useState(false);
  const [err, setErr] = useState('');

  const pwErr = tried && pw.length < 8 ? t('bt.setup_pw_short') : '';
  const pw2Err = tried && !pwErr && pw !== pw2 ? t('bt.setup_pw_mismatch') : '';

  const submit = async (e: FormEvent) => {
    e.preventDefault();
    setTried(true);
    if (pw.length < 8 || pw !== pw2) return;
    setBusy(true);
    setErr('');
    try {
      await api.bleSetup(pw, ssid.trim() ? [{ ssid: ssid.trim(), password: wpw }] : []);
      setDone(true);
    } catch (e) {
      console.warn('[eego ble] setup failed:', e);
      setErr(t('bt.setup_failed'));
    } finally {
      setBusy(false);
    }
  };

  if (done || (ble && ble.state !== 'ready')) {
    const msg = ble?.state === 'asleep' ? t('bt.asleep') : ble?.state === 'lost' ? t('bt.lost') : t('bt.rebooting');
    return (
      <Frame>
        <div className="row gap-md">
          <Spinner />
          <span>{msg}</span>
        </div>
        <Button variant="ghost" onClick={disconnectBle}>
          {t('common.cancel')}
        </Button>
      </Frame>
    );
  }

  return (
    <Frame>
      <form className="col gap-lg" onSubmit={(e) => void submit(e)}>
        <h2>{t('bt.setup_title')}</h2>
        <TextInput label={t('bt.setup_pw')} type="password" autoComplete="new-password" autoFocus value={pw} onChange={setPw} error={pwErr} />
        <TextInput label={t('bt.setup_pw2')} type="password" autoComplete="new-password" value={pw2} onChange={setPw2} error={pw2Err} />
        <h4>{t('bt.setup_wifi')}</h4>
        <TextInput label={t('bt.setup_ssid')} placeholder={t('bt.setup_optional')} value={ssid} onChange={setSsid} />
        {ssid.trim() && <TextInput label={t('bt.setup_wifi_pw')} type="password" autoComplete="off" value={wpw} onChange={setWpw} />}
        {err && <span className="err-text">{err}</span>}
        <Button type="submit" variant="primary" loading={busy} className="btn-lg">
          {t('bt.setup_submit')}
        </Button>
        <Button variant="ghost" onClick={disconnectBle}>
          {t('bt.disconnect')}
        </Button>
      </form>
    </Frame>
  );
}
