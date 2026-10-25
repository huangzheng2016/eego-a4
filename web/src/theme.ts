export type Theme = 'auto' | 'light' | 'dark';
export function loadTheme(): Theme {
  try {
    const v = localStorage.getItem('eego_theme');
    if (v === 'light' || v === 'dark') return v;
  } catch {
    /* ignore */
  }
  return 'auto';
}
export function applyTheme(t: Theme): void {
  if (t === 'auto') document.documentElement.removeAttribute('data-theme');
  else document.documentElement.setAttribute('data-theme', t);
  try {
    localStorage.setItem('eego_theme', t);
  } catch {
    /* ignore */
  }
}
