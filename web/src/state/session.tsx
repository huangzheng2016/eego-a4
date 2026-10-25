import { createContext, useCallback, useContext, useEffect, useMemo, useState, type ReactNode } from 'react';
import * as api from '../api';
import { useI18n } from '../i18n';
import { useToast } from './toast';

export type Connection = 'unknown' | 'online' | 'offline';

interface SessionCtx {
  authed: boolean;
  connection: Connection;
  status: api.DeviceStatus | null;
  login: (password: string) => Promise<void>;
  logout: () => Promise<void>;
  refreshStatus: () => Promise<void>;
}

const Ctx = createContext<SessionCtx | null>(null);

export function SessionProvider({ children }: { children: ReactNode }) {
  const [authed, setAuthed] = useState(() => !!api.getToken());
  const [connection, setConnection] = useState<Connection>('unknown');
  const [status, setStatus] = useState<api.DeviceStatus | null>(null);
  const toast = useToast();
  const { t } = useI18n();

  useEffect(
    () =>
      api.onUnauthorized(() => {
        if (api.getToken()) toast('warn', t('login.expired'));
        api.setToken(null);
        setAuthed(false);
      }),
    [toast, t],
  );
  useEffect(() => api.onConnectivity((ok) => setConnection(ok ? 'online' : 'offline')), []);

  const refreshStatus = useCallback(async () => {
    try {
      setStatus(await api.getStatus());
    } catch {
      /* connectivity listener records the failure */
    }
  }, []);

  useEffect(() => {
    if (!authed) return;
    void refreshStatus();
    const id = setInterval(() => {
      if (document.visibilityState === 'visible') void refreshStatus();
    }, 30000);
    return () => clearInterval(id);
  }, [authed, refreshStatus]);

  const login = useCallback(async (password: string) => {
    await api.login(password);
    setAuthed(true);
  }, []);
  const logout = useCallback(async () => {
    try {
      await api.logout();
    } catch {
      /* token is dropped regardless */
    }
    setAuthed(false);
    setStatus(null);
  }, []);

  const value = useMemo(() => ({ authed, connection, status, login, logout, refreshStatus }), [authed, connection, status, login, logout, refreshStatus]);
  return <Ctx.Provider value={value}>{children}</Ctx.Provider>;
}

export function useSession(): SessionCtx {
  const c = useContext(Ctx);
  if (!c) throw new Error('SessionProvider missing');
  return c;
}
