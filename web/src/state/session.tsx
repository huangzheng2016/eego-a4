import { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState, type ReactNode } from 'react';
import * as api from '../api';
import { setTransport } from '../api/transport';
import { BleLink, gattConnector, requestBleDevice, type BleState, type Connector } from '../api/ble/bleTransport';
import { setDeviceScope } from '../model/origin';
import { needsTimeSync } from '../model/timeSync';

export type Connection = 'unknown' | 'online' | 'offline';

/** BLE link as seen by the UI; null when using WiFi. */
export interface BleView {
  state: BleState;
  name: string;
  /** false: the device has no admin password yet (first-time setup over BLE). */
  provisioned: boolean;
}

async function bleConnector(): Promise<Connector> {
  if (import.meta.env.VITE_MOCK === '1') {
    const [{ loopbackDevice, fetchHandler }, { TOKEN }] = await Promise.all([import('../api/ble/loopback'), import('../api/mock')]);
    return loopbackDevice(fetchHandler(TOKEN), { v: 1, mtu: 247, name: 'EEGO-A4-MOCK', provisioned: true, httpd: true }).connector;
  }
  return gattConnector(await requestBleDevice());
}

interface SessionCtx {
  authed: boolean;
  /** The last session ended because the device rejected the token (shown on the login card). */
  expired: boolean;
  connection: Connection;
  status: api.DeviceStatus | null;
  /** performance.now() when `status` was fetched (local countdowns of remaining_s/sleep_in_s). */
  statusAt: number;
  /** Merge a fresh keep-awake state (POST /device/keep-awake response) into `status`. */
  setKeepAwake: (ka: api.DeviceStatus['keep_awake']) => void;
  login: (password: string) => Promise<void>;
  logout: () => Promise<void>;
  refreshStatus: () => Promise<void>;
  ble: BleView | null;
  /** Pick a device and connect over Web Bluetooth (call from a click). */
  connectBle: () => Promise<void>;
  disconnectBle: () => void;
}

const Ctx = createContext<SessionCtx | null>(null);

export function SessionProvider({ children }: { children: ReactNode }) {
  const [authed, setAuthed] = useState(() => !!api.getToken());
  const [expired, setExpired] = useState(false);
  const [connection, setConnection] = useState<Connection>('unknown');
  const [status, setStatus] = useState<api.DeviceStatus | null>(null);
  const [statusAt, setStatusAt] = useState(0);
  const [ble, setBle] = useState<BleView | null>(null);
  const link = useRef<BleLink | null>(null);
  const timeSynced = useRef(false);

  const disconnectBle = useCallback(() => {
    link.current?.close();
    link.current = null;
    setTransport(null);
    setDeviceScope(null);
    setBle(null);
    setAuthed(!!api.getToken());
    setStatus(null);
  }, []);

  const connectBle = useCallback(async () => {
    link.current?.close();
    const l = new BleLink(await bleConnector());
    link.current = l;
    const sync = () => {
      const info = l.info;
      setBle({ state: l.state, name: info?.name ?? '', provisioned: info?.provisioned !== false });
      // The link (a passkey-paired bond) is the authorization; no password login over BLE.
      if (l.state === 'ready' && info?.provisioned !== false) {
        setDeviceScope(info?.name);
        setExpired(false);
        setAuthed(true);
      }
    };
    l.onState(sync);
    setTransport(l);
    try {
      await l.open();
      timeSynced.current = false;
      sync();
    } catch (e) {
      l.close();
      if (link.current === l) link.current = null;
      setTransport(null);
      setBle(null);
      throw e;
    }
  }, []);

  useEffect(
    () =>
      api.onUnauthorized(() => {
        if (link.current) {
          disconnectBle();
          setExpired(true);
          return;
        }
        if (api.getToken()) setExpired(true);
        api.setToken(null);
        setAuthed(false);
      }),
    [disconnectBle],
  );
  useEffect(() => api.onConnectivity((ok) => setConnection(ok ? 'online' : 'offline')), []);

  const refreshStatus = useCallback(async () => {
    try {
      const next = await api.getStatus();
      setStatus(next);
      setStatusAt(performance.now());
      // A device without SNTP (no internet, or BLE only) takes the time from this computer once.
      if (!timeSynced.current && needsTimeSync(next.time, Date.now())) {
        timeSynced.current = true;
        void api.setDeviceTime(Date.now()).catch(() => undefined);
      }
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
    setExpired(false);
    setAuthed(true);
  }, []);
  const logout = useCallback(async () => {
    if (link.current) {
      disconnectBle();
      setAuthed(false);
      return;
    }
    try {
      await api.logout();
    } catch {
      /* token is dropped regardless */
    }
    setAuthed(false);
    setStatus(null);
  }, [disconnectBle]);

  const setKeepAwake = useCallback((ka: api.DeviceStatus['keep_awake']) => {
    setStatus((s) => (s ? { ...s, keep_awake: ka } : s));
    setStatusAt(performance.now());
  }, []);

  const value = useMemo(
    () => ({ authed, expired, connection, status, statusAt, setKeepAwake, login, logout, refreshStatus, ble, connectBle, disconnectBle }),
    [authed, expired, connection, status, statusAt, setKeepAwake, login, logout, refreshStatus, ble, connectBle, disconnectBle],
  );
  return <Ctx.Provider value={value}>{children}</Ctx.Provider>;
}

export function useSession(): SessionCtx {
  const c = useContext(Ctx);
  if (!c) throw new Error('SessionProvider missing');
  return c;
}
