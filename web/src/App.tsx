import { useEffect, useState, type FormEvent } from 'react';
import { I18nProvider, useI18n, type DictKey } from './i18n';
import { ToastProvider } from './state/toast';
import { SessionProvider, useSession } from './state/session';
import { DraftProvider, useDraft } from './state/draft';
import { navigate, useRoute, type Route } from './state/router';
import { ApiError } from './api';
import { Badge, Button, Icon, Logo, Modal, Segmented, TextInput } from './ui';
import { applyTheme, loadTheme, type Theme } from './theme';
import { EditorPage } from './pages/editor/EditorPage';
import { TemplatesPage } from './pages/TemplatesPage';
import { SourcesPage } from './pages/SourcesPage';
import { CredentialsPage } from './pages/CredentialsPage';
import { WifiPage } from './pages/WifiPage';
import { SettingsPage } from './pages/SettingsPage';
import { AlertsPage } from './pages/AlertsPage';
import { SystemPage } from './pages/SystemPage';
import { PreviewPage } from './pages/PreviewPage';
import { RulesPage } from './pages/RulesPage';
import { WelcomePage, WELCOME_SKIP_KEY } from './pages/WelcomePage';
import { KeepAwakeToggle, PowerBanner } from './pages/PowerControls';
import { probablyAsleep } from './model/power';

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
      else if (x instanceof ApiError && x.status === 0) setErr(`${t('err.network')} — ${t('err.asleep')}`);
      else setErr(t('login.failed'));
    } finally {
      setBusy(false);
    }
  };
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
        <form className="col gap-lg" onSubmit={submit}>
          <div>
            <h2>{t('login.title')}</h2>
            <p className="muted login-sub">{t('login.subtitle')}</p>
          </div>
          {!onDevice && (
            <div className="notice warn">
              <Icon name="warn" size={16} />
              <span>{t('conn.not_device')}</span>
            </div>
          )}
          <TextInput label={t('login.password')} type="password" autoComplete="current-password" autoFocus value={pw} onChange={setPw} error={err} />
          <Button type="submit" variant="primary" loading={busy} disabled={!pw} className="btn-lg">
            {t('login.submit')}
          </Button>
        </form>
      </div>
    </div>
  );
}

interface NavItem {
  route: Route;
  key: DictKey;
  icon: string;
}
const NAV_GROUPS: { key: DictKey; items: NavItem[] }[] = [
  {
    key: 'nav.group.design',
    items: [
      { route: 'editor', key: 'nav.editor', icon: 'layout' },
      { route: 'templates', key: 'nav.templates', icon: 'templates' },
      { route: 'sources', key: 'nav.sources', icon: 'db' },
      { route: 'rules', key: 'nav.rules', icon: 'rules' },
    ],
  },
  {
    key: 'nav.group.device',
    items: [
      { route: 'preview', key: 'nav.preview', icon: 'device' },
      { route: 'alerts', key: 'nav.alerts', icon: 'bell' },
      { route: 'settings', key: 'nav.settings', icon: 'sliders' },
      { route: 'wifi', key: 'nav.wifi', icon: 'wifi' },
      { route: 'credentials', key: 'nav.credentials', icon: 'key' },
      { route: 'system', key: 'nav.system', icon: 'cpu' },
    ],
  },
];
const ALL_NAV = NAV_GROUPS.flatMap((g) => g.items);
const BOTTOM: Route[] = ['editor', 'sources', 'preview', 'alerts'];

function Shell() {
  const { t, lang, setLang } = useI18n();
  const { connection, status, logout, refreshStatus } = useSession();
  const { route, arg, explicit } = useRoute();
  const draft = useDraft();
  const [sheet, setSheet] = useState(false);
  const [theme, setTheme] = useState<Theme>(loadTheme);
  useEffect(() => setSheet(false), [route]);
  useEffect(() => {
    const warn = (e: BeforeUnloadEvent) => {
      if (draft.dirty && draft.configured) e.preventDefault();
    };
    window.addEventListener('beforeunload', warn);
    return () => window.removeEventListener('beforeunload', warn);
  }, [draft.dirty, draft.configured]);

  // First run: no dashboard on the device and nothing drafted → onboarding.
  const fresh = !draft.loading && !draft.loadError && !draft.configured && !draft.restorable && draft.draft.dashboard.pages.every((p) => !p.panels.length);
  useEffect(() => {
    let skipped = false;
    try {
      skipped = sessionStorage.getItem(WELCOME_SKIP_KEY) === '1';
    } catch {
      /* ignore */
    }
    if (fresh && !skipped && (!explicit || route === 'editor')) navigate('welcome');
  }, [fresh, explicit, route]);

  const setThemeTo = (next: Theme) => {
    setTheme(next);
    applyTheme(next);
  };
  const activeAlerts = status?.alerts.available ? status.alerts.active_total ?? 0 : 0;
  // Unreachable: a battery/sleep-scheduled device is most likely asleep — say how to wake it.
  const offlineLabel = probablyAsleep(status?.power) ? `${t('conn.offline')} · ${t('conn.asleep')}` : t('conn.offline');
  const connLabel = connection === 'online' ? t('conn.online') : connection === 'offline' ? offlineLabel : t('conn.unknown');
  const badgeFor = (r: Route) =>
    r === 'editor' && draft.dirty && draft.configured ? (
      <span className="nav-pip" title={t('draft.dirty')} />
    ) : r === 'alerts' && activeAlerts > 0 ? (
      <span className="nav-badge">
        <Badge tone="danger">{activeAlerts}</Badge>
      </span>
    ) : r === 'rules' && draft.draft.dashboard.alert_rules.length > 0 ? (
      <span className="nav-badge">
        <Badge>{draft.draft.dashboard.alert_rules.length}</Badge>
      </span>
    ) : null;
  const current = ALL_NAV.find((n) => n.route === route);

  const prefs = (
    <>
      <Segmented
        size="sm"
        value={theme}
        onChange={setThemeTo}
        label={t('common.theme')}
        options={[
          { value: 'light', label: '', icon: 'sun', title: t('common.theme.light') },
          { value: 'auto', label: '', icon: 'auto', title: t('common.theme.auto') },
          { value: 'dark', label: '', icon: 'moon', title: t('common.theme.dark') },
        ]}
      />
      <Button size="sm" variant="ghost" onClick={() => setLang(lang === 'zh' ? 'en' : 'zh')} title="Language">
        <Icon name="globe" size={16} /> {lang === 'zh' ? 'EN' : '中文'}
      </Button>
    </>
  );

  return (
    <div className="shell">
      <aside className="sidebar">
        <div className="brand">
          <Logo />
          <div>
            <div className="brand-name">{t('app.brand')}</div>
            <div className="brand-sub">{t('app.tagline')}</div>
          </div>
        </div>
        {NAV_GROUPS.map((g) => (
          <nav key={g.key} aria-label={t(g.key)}>
            <div className="nav-group">{t(g.key)}</div>
            {g.items.map((n) => (
              <a key={n.route} href={`#/${n.route}`} className={`nav-link${route === n.route ? ' on' : ''}`} aria-current={route === n.route ? 'page' : undefined}>
                <Icon name={n.icon} />
                {t(n.key)}
                {badgeFor(n.route)}
              </a>
            ))}
          </nav>
        ))}
        <div className="sidebar-foot">
          <KeepAwakeToggle />
          <button
            type="button"
            className="conn"
            title={`${status ? `${status.network.ssid} · ${status.network.ip} · ` : ''}${t('conn.recheck')}`}
            onClick={() => void refreshStatus()}
          >
            <span className={`dot ${connection === 'online' ? 'ok' : connection === 'offline' ? 'bad' : ''}`} />
            <span className="grow">{connLabel}</span>
            {status && <span className="mono dim">{status.network.ip}</span>}
          </button>
          <div className="row between">{prefs}</div>
          <Button size="sm" variant="ghost" onClick={() => void logout()} className="justify-start">
            <Icon name="logout" size={16} /> {t('login.logout')}
          </Button>
        </div>
      </aside>
      <div className="main">
        <div className="topbar">
          <Logo size={26} />
          <strong className="grow truncate">{current ? t(current.key) : route === 'welcome' ? t('wz.title') : t('app.title')}</strong>
          <KeepAwakeToggle compact />
          <span className={`dot ${connection === 'online' ? 'ok' : connection === 'offline' ? 'bad' : ''}`} title={connLabel} />
        </div>
        <PowerBanner />
        {route === 'editor' && <EditorPage />}
        {route === 'templates' && <TemplatesPage />}
        {route === 'sources' && <SourcesPage sourceId={arg} />}
        {route === 'rules' && <RulesPage ruleId={arg} />}
        {route === 'credentials' && <CredentialsPage />}
        {route === 'wifi' && <WifiPage />}
        {route === 'settings' && <SettingsPage />}
        {route === 'alerts' && <AlertsPage />}
        {route === 'system' && <SystemPage />}
        {route === 'preview' && <PreviewPage />}
        {route === 'welcome' && <WelcomePage />}
      </div>
      <nav className="bottom-nav" aria-label={t('nav.menu')}>
        {BOTTOM.map((r) => {
          const n = ALL_NAV.find((x) => x.route === r)!;
          return (
            <a key={r} href={`#/${r}`} className={route === r ? 'on' : ''}>
              <Icon name={n.icon} size={22} />
              {t(`${n.key}.short` as DictKey)}
              {((r === 'alerts' && activeAlerts > 0) || (r === 'editor' && draft.dirty && draft.configured)) && <i className="pip" />}
            </a>
          );
        })}
        <button type="button" className={sheet || !BOTTOM.includes(route) ? 'on' : ''} onClick={() => setSheet(true)}>
          <Icon name="more" size={22} strokeWidth={2.6} />
          {t('nav.more')}
        </button>
      </nav>
      {sheet && (
        <>
          <div className="sheet-scrim" onClick={() => setSheet(false)} />
          <div className="sheet" role="dialog" aria-label={t('nav.more')}>
            <div className="sheet-grip" />
            <div className="sheet-grid">
              {ALL_NAV.filter((n) => !BOTTOM.includes(n.route)).map((n) => (
                <a key={n.route} href={`#/${n.route}`} className={`sheet-item${route === n.route ? ' on' : ''}`}>
                  <Icon name={n.icon} size={22} />
                  {t(n.key)}
                </a>
              ))}
              <button type="button" className="sheet-item borderless" onClick={() => void logout()}>
                <Icon name="logout" size={22} />
                {t('login.logout')}
              </button>
            </div>
            <div className="row between mt-3">{prefs}</div>
            <div className="conn mt-2">
              <span className={`dot ${connection === 'online' ? 'ok' : connection === 'offline' ? 'bad' : ''}`} />
              {connLabel} {status && <span className="mono dim">{status.network.ip}</span>}
            </div>
          </div>
        </>
      )}
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
        title={t('draft.restore_title')}
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
