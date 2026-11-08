// Where the app runs: on the device origin (loaded by the device's loader over WiFi)
// or on the GitHub Pages origin (HTTPS, where Web Bluetooth is available).
export const PAGES_URL = 'https://huangzheng2016.github.io/eego-a4/';

export const isPagesOrigin = (): boolean => typeof location !== 'undefined' && /github\.io$/.test(location.hostname);

export function bleSupported(): boolean {
  if (import.meta.env.VITE_MOCK === '1') return true;
  return typeof navigator !== 'undefined' && !!navigator.bluetooth && typeof window !== 'undefined' && window.isSecureContext;
}

// Several devices can be used from the one Pages origin, so per-device local state
// (the dashboard draft) is keyed by the BLE device name.
let scope = '';
export function setDeviceScope(name: string | null | undefined): void {
  scope = name ? `:${name}` : '';
}
export const deviceScope = (): string => scope;
