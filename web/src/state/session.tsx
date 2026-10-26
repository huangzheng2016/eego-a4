import { createContext, useCallback, useContext, useEffect, useMemo, useState, type ReactNode } from 'react';
import * as api from '../api';
import { useI18n } from '../i18n';
import { useToast } from './toast';

export type Connection = 'unknown' | 'online' | 'offline';

interface SessionCtx {
  authed: boolean;
  connection: Connection;
  status: api.DeviceStatus | null;
  /** performance.now() when `status` was fetched (local countdowns of remaining_s/sleep_in_s). */
  statusAt: number;
  /** Merge a fresh keep-awake state (POST /device/keep-awake response) into `status`. */
  setKeepAwake: (ka: api.DeviceStatus['keep_awake']) => void;
  login: (password: string) => Promise<void>;
  logout: () => Promise<void>;
  refreshStatus: () => Promise<void>;
}

const Ctx = createContext<SessionCtx | null>(null);

export function SessionProvider({ children }: { children: ReactNode }) {
  const [authed, setAuthed] = useState(() => !!api.getToken());
  const [connection, setConnection] = useState<Connection>('unknown');
  const [status, setStatus] = useState<api.DeviceStatus | null>(null);
  const [statusAt, setStatusAt] = useState(0);
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
      const next = await api.getStatus();
      setStatus(next);
      setStatusAt(performance.now());
    } catch {
      /* connectivity listener records the failure */
    }
  }, []);

  // One fetch at sign-in; afterwards only on demand (refresh buttons, after actions).
  // No background polling: on a weak WiFi link every idle request competes with
  // the saves that matter, and a failed poll flips the badge to "offline".
  useEffect(() => {
    if (authed) void refreshStatus();
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

  const setKeepAwake = useCallback((ka: api.DeviceStatus['keep_awake']) => {
    setStatus((s) => (s ? { ...s, keep_awake: ka } : s));
    setStatusAt(performance.now());
  }, []);

  const value = useMemo(
    () => ({ authed, connection, status, statusAt, setKeepAwake, login, logout, refreshStatus }),
    [authed, connection, status, statusAt, setKeepAwake, login, logout, refreshStatus],
  );
  return <Ctx.Provider value={value}>{children}</Ctx.Provider>;
}

export function useSession(): SessionCtx {
  const c = useContext(Ctx);
  if (!c) throw new Error('SessionProvider missing');
  return c;
}
