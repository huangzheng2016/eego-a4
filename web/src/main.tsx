import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import './styles.css';
import { App } from './App';

declare global {
  interface Window {
    __EEGO_APP_LOADED?: boolean;
  }
}
// Tell the device loader (/loader.js) that the bundle executed; otherwise it
// shows a static "failed to load" notice after ~8 s.
window.__EEGO_APP_LOADED = true;

async function boot() {
  if (import.meta.env.VITE_MOCK === '1') {
    const { installMock } = await import('./api/mock');
    await installMock();
  }
  const el = document.getElementById('root') ?? document.body.appendChild(document.createElement('div'));
  createRoot(el).render(
    <StrictMode>
      <App />
    </StrictMode>,
  );
}
void boot();
