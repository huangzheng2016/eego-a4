// Tiny hash router (#/editor, #/sources/...). Hash routing works when the app
// is served from the device's "/" loader page with no server-side routes.
import { useEffect, useState } from 'react';

export type Route = 'editor' | 'templates' | 'sources' | 'rules' | 'credentials' | 'wifi' | 'settings' | 'alerts' | 'system' | 'preview' | 'welcome';
export const ROUTES: Route[] = ['editor', 'templates', 'sources', 'rules', 'credentials', 'wifi', 'settings', 'alerts', 'system', 'preview', 'welcome'];

function parse(): { route: Route; arg: string; explicit: boolean } {
  const [, r = '', ...rest] = location.hash.replace(/^#/, '').split('/');
  const explicit = (ROUTES as string[]).includes(r);
  const route = explicit ? (r as Route) : 'editor';
  return { route, arg: decodeURIComponent(rest.join('/')), explicit };
}

export function navigate(route: Route, arg?: string): void {
  location.hash = `#/${route}${arg ? '/' + encodeURIComponent(arg) : ''}`;
}

export function useRoute() {
  const [state, setState] = useState(parse);
  useEffect(() => {
    const on = () => setState(parse());
    window.addEventListener('hashchange', on);
    return () => window.removeEventListener('hashchange', on);
  }, []);
  return state;
}
