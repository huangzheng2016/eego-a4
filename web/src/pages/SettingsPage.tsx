// Device settings rendered from GET /settings/schema. Capability-gated fields
// (e.g. frontlight_* when no frontlight hardware) are hidden;
// frontlight_hardware itself is always shown (under 硬件) so it can be forced.
import { useCallback, useEffect, useMemo, useState } from 'react';
import * as api from '../api';
import { useEnumLabel, useI18n, type DictKey } from '../i18n';
import { useToast } from '../state/toast';
import { errorText, fieldMap } from '../state/errors';
import { Button, Card, Checkbox, NumberInput, Select, TextArea, TextInput, Icon, PageHeader, SkeletonCard } from '../ui';

const GROUP_ORDER = ['hardware', 'display', 'refresh', 'power', 'time_language', 'ble', 'diagnostics', 'other'];
/** Client-side regrouping (schema groups are firmware-defined). */
const GROUP_OVERRIDE: Record<string, string> = { frontlight_hardware: 'hardware' };
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
    let hidden = 0;
    for (const [key, f] of Object.entries(schema?.fields ?? {})) {
      const group = GROUP_OVERRIDE[key] ?? f.group ?? 'other';
      const gate = key in GROUP_OVERRIDE ? undefined : f.capability ?? GROUP_CAPABILITY[group];
      if (gate && caps[gate] === false) {
        hidden++;
        continue;
      }
      if (!out.has(group)) out.set(group, []);
      out.get(group)!.push({ key, f });
    }
    const ordered = [...out.entries()].sort((a, b) => (GROUP_ORDER.indexOf(a[0]) + 99) % 99 - (GROUP_ORDER.indexOf(b[0]) + 99) % 99);
    return { ordered, hidden };
  }, [schema, caps]);

  const changed = useMemo(() => {
    if (!data) return {};
    const patch: Record<string, unknown> = {};
    for (const k of Object.keys(form)) if (JSON.stringify(form[k]) !== JSON.stringify(data.settings[k])) patch[k] = form[k];
    return patch;
  }, [form, data]);

  const save = async () => {
    for (const [k, txt] of Object.entries(jsonText)) {
      try {
        JSON.parse(txt);
      } catch (e) {
        setErrs({ [k]: (e as Error).message });
        return;
      }
    }
    if (!Object.keys(changed).length) return toast('info', t('set.no_changes'));
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

  const fieldLabel = (k: string) => {
    const key = `set.f.${k}` as DictKey;
    const s = t(key);
    return s === key ? k : s;
  };

  const renderField = (key: string, f: api.SettingsFieldSchema) => {
    const v = form[key];
    const set = (nv: unknown) => setForm((s) => ({ ...s, [key]: nv }));
    const hint = f.range ? `${f.range[0]} – ${f.range[1]}${f.minimum_nonzero ? ` (0 / ≥${f.minimum_nonzero})` : ''}` : f.note;
    const common = { label: fieldLabel(key), error: errs[key] };
    if (f.type === 'bool') return <Checkbox key={key} {...common} checked={!!v} onChange={set} />;
    if (f.enum) return <Select key={key} {...common} hint={f.note} value={String(v ?? '')} options={f.enum.map((e) => ({ value: e, label: enumLabel(`set.e.${key}`, e) }))} onChange={set} />;
    if (key === 'timezone_offset_min') return <Select key={key} {...common} value={String(v ?? 0)} options={TZ_OPTIONS} onChange={(x) => set(Number(x))} />;
    if (key.endsWith('_min') && f.range?.[0] === -1)
      return (
        <TextInput
          key={key}
          {...common}
          type="time"
          hint={t('set.minutes_disabled')}
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

  if (!schema || !data) {
    return (
      <div className="page stack">
        <PageHeader icon="sliders" title={t('set.title')} />
        <SkeletonCard lines={4} />
        <SkeletonCard lines={3} />
      </div>
    );
  }
  const dirty = Object.keys(changed).length > 0;
  return (
    <div className="page stack">
      <PageHeader
        icon="sliders"
        title={t('set.title')}
        subtitle={t('set.subtitle')}
        actions={
          <>
            <Button onClick={() => void load()}>
              <Icon name="refresh" size={16} /> {t('common.reload')}
            </Button>
            <Button variant="primary" loading={busy} disabled={!dirty} onClick={() => void save()}>
              {t('set.save')}
            </Button>
          </>
        }
      />
      {groups.hidden > 0 && <div className="dim">{t('set.capability_off', { n: groups.hidden })}</div>}
      {groups.ordered.map(([group, fields]) => {
        const titleKey = `set.group.${group}` as DictKey;
        return (
          <Card key={group} title={t(titleKey) === titleKey ? group : t(titleKey)}>
            <div className="form-grid two">{fields.map(({ key, f }) => renderField(key, f))}</div>
            {group === 'ble' && caps.bluetooth && (
              <div style={{ marginTop: 10 }}>
                <Button
                  size="sm"
                  variant="danger"
                  onClick={async () => {
                    if (!confirm(t('set.ble_unbind_confirm'))) return;
                    try {
                      await api.bleUnbind();
                      toast('ok', t('set.ble_unbound'));
                    } catch (e) {
                      toast('error', errorText(e, t));
                    }
                  }}
                >
                  {t('set.ble_unbind')}
                </Button>
              </div>
            )}
          </Card>
        );
      })}
      <Card title={t('set.status')}>
        <div className="row" style={{ marginBottom: 8 }}>
          {Object.entries(caps).map(([k, on]) => (
            <span key={k} className={`badge ${on ? 'ok' : ''}`}>
              {on ? '✓' : '✗'} {k}
            </span>
          ))}
        </div>
        <details>
          <summary className="dim">power / ble</summary>
          <pre>{JSON.stringify({ power: data.power, ble: data.ble }, null, 2)}</pre>
        </details>
      </Card>
    </div>
  );
}
