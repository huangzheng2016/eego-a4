// Device settings rendered from GET /settings/schema. Capability-gated fields
// (e.g. frontlight_* when no frontlight hardware) are hidden;
// frontlight_hardware itself is always shown so it can be forced.
import { useCallback, useEffect, useMemo, useState } from 'react';
import * as api from '../api';
import { useEnumLabel, useI18n, type DictKey } from '../i18n';
import { useToast } from '../state/toast';
import { errorText, fieldMap } from '../state/errors';
import { Button, Card, Checkbox, NumberInput, Select, TextArea, TextInput, PageHeader, SkeletonCard, useConfirm } from '../ui';

const GROUP_ORDER = ['display', 'refresh', 'power', 'battery', 'time_language', 'ble', 'diagnostics', 'other'];
/** Client-side regrouping (schema groups are firmware-defined). */
const GROUP_MERGE: Record<string, string> = { hardware: 'display', display_refresh: 'refresh' };
/** Fields shown even when their capability is off (so detection can be overridden). */
const ALWAYS_SHOWN = new Set(['frontlight_hardware']);
/** Seconds presets for the periodic full refresh (instead of a 0–604800 number box). */
const FULL_REFRESH_PRESETS = [0, 21600, 43200, 86400, 604800];
/** Group-level capability gates in addition to per-field `capability`. */
const GROUP_CAPABILITY: Record<string, string> = { ble: 'ble_observer' };

export function minutesToHHMM(v: unknown): string {
  const n = Number(v);
  if (!Number.isFinite(n) || n < 0) return '';
  return `${String(Math.floor(n / 60)).padStart(2, '0')}:${String(n % 60).padStart(2, '0')}`;
}
export function hhmmToMinutes(s: string): number | null {
  if (!s) return -1;
  const m = /^([01]\d|2[0-3]):([0-5]\d)$/.exec(s);
  return m ? Number(m[1]) * 60 + Number(m[2]) : null;
}
function tzLabel(min: number): string {
  const sign = min < 0 ? '-' : '+';
  const a = Math.abs(min);
  return `UTC${sign}${String(Math.floor(a / 60)).padStart(2, '0')}:${String(a % 60).padStart(2, '0')}`;
}
const TZ_OPTIONS = Array.from({ length: (840 * 2) / 15 + 1 }, (_, i) => -840 + i * 15).map((m) => ({ value: String(m), label: tzLabel(m) }));

export function SettingsPage() {
  const { t } = useI18n();
  const enumLabel = useEnumLabel();
  const toast = useToast();
  const [schema, setSchema] = useState<api.SettingsSchema | null>(null);
  const [data, setData] = useState<api.SettingsResponse | null>(null);
  const [form, setForm] = useState<Record<string, unknown>>({});
  const [jsonText, setJsonText] = useState<Record<string, string>>({});
  const [errs, setErrs] = useState<Record<string, string>>({});
  const [busy, setBusy] = useState(false);
  const [unbinding, setUnbinding] = useState(false);
  const [confirm, confirmDialog] = useConfirm();

  const adopt = useCallback((r: api.SettingsResponse) => {
    setData(r);
    setForm(structuredClone(r.settings));
    setJsonText({});
  }, []);
  const load = useCallback(async () => {
    try {
      const [s, r] = await Promise.all([api.getSettingsSchema(), api.getSettings()]);
      setSchema(s);
      adopt(r);
    } catch (e) {
      toast('error', errorText(e, t));
    }
  }, [adopt, toast, t]);
  useEffect(() => {
    void load();
  }, [load]);

  const caps = useMemo(() => data?.capabilities ?? {}, [data]);
  const groups = useMemo(() => {
    const out = new Map<string, { key: string; f: api.SettingsFieldSchema }[]>();
    for (const [key, f] of Object.entries(schema?.fields ?? {})) {
      const raw = f.group ?? 'other';
      const group = GROUP_MERGE[raw] ?? raw;
      const gate = ALWAYS_SHOWN.has(key) ? undefined : f.capability ?? GROUP_CAPABILITY[group];
      if (gate && caps[gate] === false) continue;
      if (!out.has(group)) out.set(group, []);
      out.get(group)!.push({ key, f });
    }
    return [...out.entries()].sort((a, b) => (GROUP_ORDER.indexOf(a[0]) + 99) % 99 - (GROUP_ORDER.indexOf(b[0]) + 99) % 99);
  }, [schema, caps]);

  const changed = useMemo(() => {
    if (!data) return {};
    const patch: Record<string, unknown> = {};
    for (const k of Object.keys(form)) if (JSON.stringify(form[k]) !== JSON.stringify(data.settings[k])) patch[k] = form[k];
    return patch;
  }, [form, data]);

  const pending = Object.keys(changed).length > 0;
  useEffect(() => {
    if (!pending) return;
    const warn = (e: BeforeUnloadEvent) => e.preventDefault();
    window.addEventListener('beforeunload', warn);
    return () => window.removeEventListener('beforeunload', warn);
  }, [pending]);

  const save = async () => {
    for (const [k, txt] of Object.entries(jsonText)) {
      try {
        JSON.parse(txt);
      } catch {
        setErrs({ [k]: t('set.json_invalid') });
        return;
      }
    }
    setBusy(true);
    setErrs({});
    try {
      await api.putSettings(changed);
      // Re-fetch so capability gating (e.g. frontlight_hardware) updates.
      adopt(await api.getSettings());
      toast('ok', t('set.saved'));
    } catch (e) {
      setErrs(fieldMap(e));
      toast('error', errorText(e, t));
    } finally {
      setBusy(false);
    }
  };

  const discard = async () => {
    if (!(await confirm({ title: t('set.discard'), body: t('set.discard_confirm', { n: Object.keys(changed).length }), ok: t('set.discard'), danger: true }))) return;
    if (data) adopt(data);
    setErrs({});
  };
  const unbind = async () => {
    if (!(await confirm({ title: t('set.ble_unbind'), body: t('set.ble_unbind_confirm'), ok: t('set.ble_unbind'), danger: true }))) return;
    setUnbinding(true);
    try {
      await api.bleUnbind();
      toast('ok', t('set.ble_unbound'));
    } catch (e) {
      toast('error', errorText(e, t));
    } finally {
      setUnbinding(false);
    }
  };
  const fieldLabel = (k: string) => {
    const key = `set.f.${k}` as DictKey;
    const s = t(key);
    return s === key ? k : s;
  };
  /** Optional localized help text (`set.h.<field>`); undefined when absent. */
  const fieldHelp = (k: string) => {
    const key = `set.h.${k}` as DictKey;
    const s = t(key);
    return s === key ? undefined : s;
  };

  const renderField = (key: string, f: api.SettingsFieldSchema) => {
    const v = form[key];
    const set = (nv: unknown) => setForm((s) => ({ ...s, [key]: nv }));
    const help = fieldHelp(key);
    const range = f.range ? t('set.range', { min: f.range[0], max: f.range[1] }) + (f.minimum_nonzero ? t('set.range_nonzero', { n: f.minimum_nonzero }) : '') : '';
    const hint = [help, range].filter(Boolean).join(' · ') || undefined;
    const common = { label: fieldLabel(key), error: errs[key] };
    if (f.type === 'bool') return <Checkbox key={key} {...common} hint={help} checked={!!v} onChange={set} />;
    if (f.enum) {
      const numeric = f.type === 'int' || f.type === 'number';
      return (
        <Select
          key={key}
          {...common}
          hint={help}
          value={String(v ?? '')}
          options={f.enum.map((e) => ({ value: String(e), label: enumLabel(`set.e.${key}`, String(e)) }))}
          onChange={(x) => set(numeric ? Number(x) : x)}
        />
      );
    }
    if (key === 'epd_full_refresh_s')
      return (
        <Select
          key={key}
          {...common}
          hint={help}
          value={String(v ?? 0)}
          options={FULL_REFRESH_PRESETS.map((n) => ({ value: String(n), label: t(`set.e.epd_full_refresh_s.${n}` as DictKey) }))}
          onChange={(x) => set(Number(x))}
        />
      );
    if (key === 'timezone_offset_min') return <Select key={key} {...common} value={String(v ?? 0)} options={TZ_OPTIONS} onChange={(x) => set(Number(x))} />;
    if (key.endsWith('_min') && f.range?.[0] === -1)
      return (
        <TextInput
          key={key}
          {...common}
          type="time"
          hint={help ? `${help} · ${t('set.minutes_disabled')}` : t('set.minutes_disabled')}
          value={minutesToHHMM(v)}
          onChange={(x) => {
            const m = hhmmToMinutes(x);
            if (m !== null) set(m);
          }}
        />
      );
    if (f.type === 'array')
      return (
        <TextArea
          key={key}
          {...common}
          hint={t('set.rules_json')}
          rows={6}
          value={jsonText[key] ?? JSON.stringify(v ?? [], null, 2)}
          onChange={(x) => {
            setJsonText((s) => ({ ...s, [key]: x }));
            try {
              set(JSON.parse(x));
            } catch {
              /* keep text until valid */
            }
          }}
        />
      );
    return <NumberInput key={key} {...common} hint={hint} min={f.range?.[0]} max={f.range?.[1]} step={f.step} value={typeof v === 'number' ? v : undefined} onChange={(x) => x !== undefined && set(Math.round(x))} />;
  };

  const dirty = pending;
  if (!schema || !data) {
    return (
      <div className="page stack">
        <PageHeader icon="sliders" title={t('set.title')} />
        <SkeletonCard lines={4} />
        <SkeletonCard lines={3} />
      </div>
    );
  }
  return (
    <div className="page stack">
      <PageHeader icon="sliders" title={t('set.title')} />
      {groups.map(([group, fields]) => {
        const titleKey = `set.group.${group}` as DictKey;
        return (
          <Card key={group} title={t(titleKey) === titleKey ? group : t(titleKey)}>
            <div className="form-grid two">{fields.map(({ key, f }) => renderField(key, f))}</div>
            {group === 'display' && <p className="dim card-foot-note">{t('set.touch_cal_note')}</p>}
            {group === 'ble' && caps.bluetooth && (
              <div className="card-foot-note">
                <Button size="sm" variant="danger" loading={unbinding} onClick={() => void unbind()}>
                  {t('set.ble_unbind')}
                </Button>
              </div>
            )}
          </Card>
        );
      })}
      {dirty && (
        <div className="save-bar" role="region" aria-label={t('set.save')}>
          <span className="grow">{t('set.unsaved', { n: Object.keys(changed).length })}</span>
          <Button disabled={busy} onClick={() => void discard()}>
            {t('set.discard')}
          </Button>
          <Button variant="primary" loading={busy} onClick={() => void save()}>
            {t('set.save')}
          </Button>
        </div>
      )}
      {confirmDialog}
    </div>
  );
}
