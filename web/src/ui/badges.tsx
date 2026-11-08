import type { ReactNode } from 'react';
import { useEnumLabel, useI18n } from '../i18n';
import { Badge } from './index';

export type SeverityTone = 'danger' | 'warn' | undefined;

export function severityTone(sev?: string): SeverityTone {
  return sev === 'critical' ? 'danger' : sev === 'warning' ? 'warn' : undefined;
}

export function SeverityBadge({ sev }: { sev: string }) {
  const label = useEnumLabel();
  return <Badge tone={severityTone(sev)}>{label('severity', sev)}</Badge>;
}

export function HttpStatusBadge({ status, children }: { status: number; children: ReactNode }) {
  return <Badge tone={status >= 200 && status < 300 ? 'ok' : 'danger'}>{children}</Badge>;
}

/** Relative time ("3 分钟前") for a unix-seconds timestamp; absolute time in the tooltip. */
export function RelTime({ ts }: { ts?: number }) {
  const { lang } = useI18n();
  if (!ts) return <span className="dim">—</span>;
  const ms = ts * 1000;
  return <span title={new Date(ms).toLocaleString(lang === 'zh' ? 'zh-CN' : 'en')}>{relativeTime(ms, lang)}</span>;
}

export function relativeTime(ms: number, lang: string, now = Date.now()): string {
  const fmt = new Intl.RelativeTimeFormat(lang === 'zh' ? 'zh-CN' : 'en', { numeric: 'auto' });
  const s = Math.round((ms - now) / 1000);
  const abs = Math.abs(s);
  if (abs < 45) return fmt.format(0, 'second');
  if (abs < 3600) return fmt.format(Math.round(s / 60), 'minute');
  if (abs < 86400) return fmt.format(Math.round(s / 3600), 'hour');
  if (abs < 86400 * 7) return fmt.format(Math.round(s / 86400), 'day');
  return new Date(ms).toLocaleDateString(lang === 'zh' ? 'zh-CN' : 'en');
}
