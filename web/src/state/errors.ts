import { ApiError } from '../api';
import type { TFn } from '../i18n';

/** Human message for an API failure. */
export function errorText(e: unknown, t: TFn): string {
  if (e instanceof ApiError) {
    // Unreachable: most often a battery device in deep sleep (HTTP is down while asleep).
    if (e.status === 0) return `${t(e.code === 'timeout' ? 'err.timeout' : 'err.network')} — ${t('err.asleep')}`;
    if (e.status === 409) return t('err.conflict');
    if (e.status === 429) return t('err.rate') + (e.retryAfter ? ` (${e.retryAfter}s)` : '');
    if (e.status === 413) return t('err.too_large');
    if (e.code === 'validation_failed' && e.fields.length) {
      return t('err.validation') + '\n' + e.fields.slice(0, 6).map((f) => `${f.field}: ${f.message}`).join('\n');
    }
    return t('err.generic', { msg: e.message });
  }
  return t('err.generic', { msg: e instanceof Error ? e.message : String(e) });
}

export function fieldMap(e: unknown): Record<string, string> {
  if (!(e instanceof ApiError)) return {};
  return Object.fromEntries(e.fields.map((f) => [f.field, f.message]));
}
