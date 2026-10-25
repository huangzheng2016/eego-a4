import { useEffect, useState, type FormEvent } from 'react';
import { I18nProvider, useI18n, type DictKey } from './i18n';
import { ToastProvider } from './state/toast';
import { SessionProvider, useSession } from './state/session';
import { DraftProvider, useDraft } from './state/draft';
import { useRoute, type Route } from './state/router';
import { ApiError } from './api';
import { Badge, Button, Card, Icon, Modal, TextInput } from './ui';
import { applyTheme, loadTheme, type Theme } from './theme';
import { EditorPage } from './pages/editor/EditorPage';
import { TemplatesPage } from './pages/TemplatesPage';
import { SourcesPage } from './pages/SourcesPage';
import { CredentialsPage } from './pages/CredentialsPage';
import { SettingsPage } from './pages/SettingsPage';
import { AlertsPage } from './pages/AlertsPage';
import { SystemPage } from './pages/SystemPage';
import { PreviewPage } from './pages/PreviewPage';

export function App() {
  return (
    <I18nProvider>
      <ToastProvider>
        <SessionProvider>
          <Gate />
        </SessionProvider>
      </ToastProvider>
    </I18nProvider>
  );
}

function Gate() {
  const { authed } = useSession();
  const { t } = useI18n();
  useEffect(() => {
    document.title = t('app.title');
  }, [t]);
  useEffect(() => applyTheme(loadTheme()), []);
  if (!authed) return <Login />;
  return (
    <DraftProvider>
      <Shell />
    </DraftProvider>
  );
}

function Login() {
  const { t } = useI18n();
  const { login } = useSession();
  const [pw, setPw] = useState('');
  const [err, setErr] = useState('');
  const [busy, setBusy] = useState(false);
  const onDevice = !/github\.io$/.test(location.hostname);
  const submit = async (e: FormEvent) => {
    e.preventDefault();
    setBusy(true);
    setErr('');
    try {
      await login(pw);
      setPw('');
    } catch (x) {
      if (x instanceof ApiError && x.status === 429) setErr(t('login.rate', { s: x.retryAfter ?? 60 }));
      else if (x instanceof ApiError && x.status === 0) setErr(t('err.network'));
      else setErr(t('login.failed'));
    } finally {
      setBusy(false);
    }
  };
  return (
    <div className="login">
      <Card title={t('login.title')}>
        <form className="col" onSubmit={submit}>
          {!onDevice && <div className="notice warn">{t('conn.not_device')}</div>}
          <TextInput label={t('login.password')} type="password" autoComplete="current-password" autoFocus value={pw} onChange={setPw} error={err} />
          <Button type="submit" variant="primary" loading={busy} disabled={!pw}>
            {t('login.submit')}
          </Button>
          <a className="dim" href="/legacy">
            {t('login.legacy')}
          </a>
        </form>
      </Card>
    </div>
  );
}

const NAV: { route: Route; key: DictKey; icon: string }[] = [
  { route: 'editor', key: 'nav.editor', icon: 'layout' },
  { route: 'templates', key: 'nav.templates', icon: 'grid' },
  { route: 'sources', key: 'nav.sources', icon: 'db' },
  { route: 'credentials', key: 'nav.credentials', icon: 'key' },
  { route: 'settings', key: 'nav.settings', icon: 'gear' },
  { route: 'alerts', key: 'nav.alerts', icon: 'bell' },
  { route: 'system', key: 'nav.system', icon: 'cpu' },
  { route: 'preview', key: 'nav.preview', icon: 'eye' },
];

function Shell() {
  const { t, lang, setLang } = useI18n();
  const { connection, status, logout } = useSession();
  const { route, arg } = useRoute();
  const draft = useDraft();
  const [navOpen, setNavOpen] = useState(false);
  const [theme, setTheme] = useState<Theme>(loadTheme);
  useEffect(() => setNavOpen(false), [route]);
  useEffect(() => {
    const warn = (e: BeforeUnloadEvent) => {
      if (draft.dirty && draft.configured) e.preventDefault();
    };
    window.addEventListener('beforeunload', warn);
    return () => window.removeEventListener('beforeunload', warn);
  }, [draft.dirty, draft.configured]);

  const cycleTheme = () => {
    const next: Theme = theme === 'auto' ? 'light' : theme === 'light' ? 'dark' : 'auto';
    setTheme(next);
    applyTheme(next);
  };
  const activeAlerts = status?.alerts.available ? status.alerts.active_total ?? 0 : 0;
  const connLabel = connection === 'online' ? t('conn.online') : connection === 'offline' ? t('conn.offline') : t('conn.unknown');

  return (
    <div className={`shell${navOpen ? ' nav-open' : ''}`}>
      {navOpen && <div className="scrim" onClick={() => setNavOpen(false)} />}
      <aside className="sidebar">
        <div className="brand">{t('app.brand')}</div>
        {NAV.map((n) => (
          <a key={n.route} href={`#/${n.route}`} className={`nav-link${route === n.route ? ' on' : ''}`}>
            <Icon name={n.icon} />
            {t(n.key)}
            {n.route === 'editor' && draft.dirty && draft.configured && <span className="nav-badge"><Badge tone="warn">●</Badge></span>}
            {n.route === 'alerts' && activeAlerts > 0 && <span className="nav-badge"><Badge tone="danger">{activeAlerts}</Badge></span>}
          </a>
        ))}
        <div className="sidebar-foot">
          <div className="row dim" title={status ? `${status.network.ssid} · ${status.network.ip}` : ''}>
            <span className={`dot ${connection === 'online' ? 'ok' : connection === 'offline' ? 'bad' : ''}`} />
            {connLabel}
            {status && <span className="mono">{status.network.ip}</span>}
          </div>
          <div className="row">
            <Button size="sm" onClick={() => setLang(lang === 'zh' ? 'en' : 'zh')}>
              <Icon name="globe" /> {lang === 'zh' ? 'EN' : '中文'}
            </Button>
            <Button size="sm" onClick={cycleTheme} title={t('common.theme')}>
              <Icon name="sun" /> {t(`common.theme.${theme}` as DictKey)}
            </Button>
          </div>
          <Button size="sm" variant="ghost" onClick={() => void logout()}>
            <Icon name="logout" /> {t('login.logout')}
          </Button>
        </div>
      </aside>
      <div className="main">
        <div className="topbar">
          <Button icon variant="ghost" aria-label={t('nav.menu')} onClick={() => setNavOpen(true)}>
            <Icon name="menu" />
          </Button>
          <strong className="grow">{t(NAV.find((n) => n.route === route)?.key ?? 'app.title')}</strong>
          <span className={`dot ${connection === 'online' ? 'ok' : connection === 'offline' ? 'bad' : ''}`} title={connLabel} />
        </div>
        {route === 'editor' && <EditorPage />}
        {route === 'templates' && <TemplatesPage />}
        {route === 'sources' && <SourcesPage sourceId={arg} />}
        {route === 'credentials' && <CredentialsPage />}
        {route === 'settings' && <SettingsPage />}
        {route === 'alerts' && <AlertsPage />}
        {route === 'system' && <SystemPage />}
        {route === 'preview' && <PreviewPage />}
      </div>
      <DraftDialogs />
    </div>
  );
}

function DraftDialogs() {
  const { t } = useI18n();
  const d = useDraft();
  if (d.conflict) {
    return (
      <Modal
        title={t('conflict.title')}
        onClose={() => void d.resolveConflict('cancel')}
        footer={
          <>
            <Button onClick={() => void d.resolveConflict('cancel')}>{t('common.cancel')}</Button>
            <Button onClick={() => void d.resolveConflict('reload')}>{t('conflict.reload')}</Button>
            <Button variant="primary" onClick={() => void d.resolveConflict('overwrite')}>
              {t('conflict.overwrite')}
            </Button>
          </>
        }
      >
        <p>{t('conflict.body', { rev: d.conflict.remoteRevision })}</p>
      </Modal>
    );
  }
  if (d.restorable) {
    return (
      <Modal
        title={t('draft.dirty')}
        onClose={() => d.restore(false)}
        footer={
          <>
            <Button onClick={() => d.restore(false)}>{t('draft.restore_no')}</Button>
            <Button variant="primary" onClick={() => d.restore(true)}>
              {t('draft.restore_yes')}
            </Button>
          </>
        }
      >
        <p>{t('draft.restore', { rev: d.restorable.base_revision })}</p>
      </Modal>
    );
  }
  return null;
}
