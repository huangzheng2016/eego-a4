// Alert rules editor (dashboard.alert_rules, dashboard_core model.h AlertRule):
// list + form with an expression helper, live syntax check and a preview of
// the rule against the latest known metric value. Rules are part of the
// dashboard draft and take effect after "应用到设备".
import { useMemo, useRef, useState, type ReactNode } from 'react';
import { useEnumLabel, useI18n } from '../i18n';
import { useDraft } from '../state/draft';
import { navigate } from '../state/router';
import { uniqueId } from '../model/bundle';
import { checkExpression, evaluateExpression, EXPRESSION_FUNCTIONS, identifiers, ruleFires } from '../model/expression';
import { metricsForSource } from '../model/metrics';
import { formatValue, latestValue } from '../model/series';
import { SEVERITIES, type AlertRule, type AlertSurface, type Bundle, type Severity } from '../model/types';
import { Badge, Button, EmptyState, Field, Icon, NumberInput, PageHeader, Segmented, Select, Switch, TextArea, TextInput } from '../ui';
import { DraftToolbar } from './editor/EditorPage';

const SURFACES: AlertSurface[] = ['banner', 'card', 'table_row', 'sparkline'];
const OPS = ['>', '>=', '<', '<=', '==', '!='] as const;
const DURATIONS = [0, 60000, 300000, 900000];

function ruleErrors(errors: { field: string; message: string }[], index: number): Record<string, string> {
  const out: Record<string, string> = {};
  const re = new RegExp(`^(?:dashboard\\.)?alert_rules\\[${index}\\]\\.?(.*)$`);
  for (const e of errors) {
    const m = re.exec(e.field);
    if (m) out[m[1] || '_'] = e.message;
  }
  return out;
}

type Status = { kind: 'fire' | 'calm' | 'nodata' | 'error' | 'off'; value?: number; unit?: string; error?: string; live?: boolean };

function ruleStatus(b: Bundle, observed: Parameters<typeof latestValue>[1], r: AlertRule): Status {
  if (r.enabled === false) return { kind: 'off' };
  const syntax = checkExpression(r.expression ?? '');
  if (syntax) return { kind: 'error', error: syntax.message };
  const v = latestValue(b, observed, r.source_id ?? '', r.metric ?? '');
  if (!v) return { kind: 'nodata' };
  const res = evaluateExpression(r.expression ?? '', { value: v.value, now: Date.now() });
  if (!res.ok) return { kind: 'error', error: res.error, value: v.value, unit: v.unit };
  return { kind: ruleFires(res.value) ? 'fire' : 'calm', value: v.value, unit: v.unit, live: v.live };
}

/** Parse "value OP number" so the quick builder can edit simple rules. */
function simpleForm(expr: string): { op: (typeof OPS)[number]; n: number } | null {
  const m = /^\s*value\s*(>=|<=|==|!=|>|<)\s*(-?\d+(?:\.\d+)?)\s*$/.exec(expr);
  return m ? { op: m[1] as (typeof OPS)[number], n: Number(m[2]) } : null;
}

export function RulesPage({ ruleId }: { ruleId: string }) {
  const { t } = useI18n();
  const label = useEnumLabel();
  const d = useDraft();
  const rules = d.draft.dashboard.alert_rules;
  const sources = d.draft.dashboard.sources;
  const [selected, setSelected] = useState<string | null>(ruleId || null);
  const sel = rules.find((r) => r.id === selected) ?? rules[0] ?? null;

  const create = (preset?: Partial<AlertRule>) => {
    const src = sources[0]?.id ?? '';
    const metric = src ? metricsForSource(d.draft, src, d.observed)[0]?.name ?? '' : '';
    const id = uniqueId(preset?.id ?? 'rule', rules.map((r) => r.id));
    const rule: AlertRule = {
      id,
      title: preset?.title ?? t('rules.new_title'),
      enabled: true,
      source_id: src,
      metric,
      expression: preset?.expression ?? 'value > 80',
      for_ms: preset?.for_ms ?? 0,
      message: preset?.message ?? '',
      style: { surfaces: ['banner', 'card'], severity: preset?.style?.severity ?? 'warning', linked_panel_id: '' },
    };
    d.update((b) => b.dashboard.alert_rules.push(rule));
    setSelected(id);
  };

  const presets: { key: string; icon: string; rule: Partial<AlertRule> }[] = [
    { key: 'rules.preset.high', icon: 'up', rule: { id: 'high', title: t('rules.preset.high'), expression: 'value > 80', for_ms: 300000, style: { severity: 'warning' } } },
    { key: 'rules.preset.critical', icon: 'warn', rule: { id: 'critical', title: t('rules.preset.critical'), expression: 'value >= 95', for_ms: 60000, style: { severity: 'critical' } } },
    { key: 'rules.preset.low', icon: 'down', rule: { id: 'low', title: t('rules.preset.low'), expression: 'value < 10', for_ms: 0, style: { severity: 'info' } } },
    { key: 'rules.preset.range', icon: 'shield', rule: { id: 'range', title: t('rules.preset.range'), expression: 'value < 18 || value > 28', for_ms: 600000, style: { severity: 'warning' } } },
  ];

  return (
    <div className="page wide">
      <PageHeader
        icon="rules"
        title={t('rules.title')}
        subtitle={t('rules.subtitle')}
        actions={
          <Button variant="primary" onClick={() => create()}>
            <Icon name="plus" size={16} /> {t('rules.add')}
          </Button>
        }
      />
      <div className="card row toolbar-card">
        <DraftToolbar compact />
      </div>
      {!sources.length && (
        <div className="notice warn mb-4">
          <Icon name="warn" size={16} />
          <span className="grow">{t('rules.no_sources')}</span>
          <Button size="sm" onClick={() => navigate('sources')}>
            {t('nav.sources')} <Icon name="arrowr" size={14} />
          </Button>
        </div>
      )}
      {!rules.length ? (
        <div className="card">
          <EmptyState icon="rules" title={t('rules.empty_title')} action={<Button variant="primary" onClick={() => create()}><Icon name="plus" size={16} /> {t('rules.add')}</Button>}>
            {t('rules.empty_desc')}
          </EmptyState>
          <div className="card-body border-top">
            <h4 className="mb-3">{t('rules.presets')}</h4>
            <div className="tiles">
              {presets.map((p) => (
                <button key={p.key} type="button" className="card tile tile-btn" onClick={() => create(p.rule)}>
                  <span className="tile-label">
                    <Icon name={p.icon} size={15} /> {label('severity', p.rule.style!.severity!)}
                  </span>
                  <strong>{p.rule.title}</strong>
                  <code className="expr self-start">{p.rule.expression}</code>
                </button>
              ))}
            </div>
          </div>
        </div>
      ) : (
        <div className="rules-layout">
          <div className="rule-list">
            {rules.map((r, i) => {
              const st = ruleStatus(d.draft, d.observed, r);
              const errs = Object.keys(ruleErrors(d.errors, i)).length;
              const sev = r.style?.severity ?? 'info';
              return (
                <div key={r.id} className={`card rule${r.id === sel?.id ? ' on' : ''}${r.enabled === false ? ' off' : ''}`} role="button" tabIndex={0} onClick={() => setSelected(r.id)} onKeyDown={(e) => e.key === 'Enter' && setSelected(r.id)}>
                  <span className={`rule-strip ${sev}`} />
                  <div className="col gap-xs min-w-0">
                    <div className="row nowrap">
                      <strong className="truncate">{r.title || r.id}</strong>
                      <Badge tone={sev === 'critical' ? 'danger' : sev === 'warning' ? 'warn' : 'accent'}>{label('severity', sev)}</Badge>
                      {errs > 0 && <Badge tone="danger">{t('rules.errors', { n: errs })}</Badge>}
                    </div>
                    <div className="row nowrap dim min-w-0">
                      <span className="truncate mono">
                        {r.source_id || '—'} / {r.metric || '—'}
                      </span>
                      <code className="expr truncate">{r.expression}</code>
                      {(r.for_ms ?? 0) > 0 && (
                        <span className="row nowrap gap-xs">
                          <Icon name="clock" size={13} /> {fmtDuration(r.for_ms ?? 0, t)}
                        </span>
                      )}
                    </div>
                  </div>
                  <StatusBadge st={st} />
                </div>
              );
            })}
          </div>
          {sel ? (
            <RuleEditor key={sel.id} rule={sel} index={rules.indexOf(sel)} onSelect={setSelected} />
          ) : (
            <div className="card">
              <EmptyState icon="rules" title={t('rules.pick')} />
            </div>
          )}
        </div>
      )}
    </div>
  );
}

function fmtDuration(ms: number, t: ReturnType<typeof useI18n>['t']): string {
  if (ms === 0) return t('rules.for.now');
  if (ms % 3600000 === 0) return t('rules.hours', { n: ms / 3600000 });
  if (ms % 60000 === 0) return t('rules.minutes', { n: ms / 60000 });
  return t('rules.seconds', { n: Math.round(ms / 1000) });
}

function StatusBadge({ st }: { st: Status }) {
  const { t } = useI18n();
  if (st.kind === 'off') return <Badge>{t('rules.status.off')}</Badge>;
  if (st.kind === 'error') return <Badge tone="danger">{t('rules.status.error')}</Badge>;
  if (st.kind === 'nodata') return <Badge>{t('rules.status.nodata')}</Badge>;
  return (
    <Badge tone={st.kind === 'fire' ? 'danger' : 'ok'} dot>
      {st.kind === 'fire' ? t('rules.status.fire') : t('rules.status.calm')}
    </Badge>
  );
}

function Group({ title, children, hint }: { title: string; children: ReactNode; hint?: ReactNode }) {
  return (
    <div className="section">
      <h4>{title}</h4>
      {hint && <p className="dim section-hint">{hint}</p>}
      {children}
    </div>
  );
}

function RuleEditor({ rule: r, index, onSelect }: { rule: AlertRule; index: number; onSelect: (id: string | null) => void }) {
  const { t } = useI18n();
  const label = useEnumLabel();
  const d = useDraft();
  const exprRef = useRef<HTMLTextAreaElement>(null);
  const [trial, setTrial] = useState<number | undefined>(undefined);
  const errs = useMemo(() => ruleErrors(d.errors, index), [d.errors, index]);
  const dash = d.draft.dashboard;
  const set = (fn: (x: AlertRule) => void, key = 'x') =>
    d.update((b) => {
      const x = b.dashboard.alert_rules.find((y) => y.id === r.id);
      if (x) fn(x);
    }, `rule:${r.id}:${key}`);
  const setStyle = (patch: Partial<NonNullable<AlertRule['style']>>) => set((x) => (x.style = { surfaces: ['card'], severity: 'info', linked_panel_id: '', ...x.style, ...patch }), 'style');

  const metrics = r.source_id ? metricsForSource(d.draft, r.source_id, d.observed) : [];
  const syntax = checkExpression(r.expression ?? '');
  const unknownIds = syntax ? [] : identifiers(r.expression ?? '').filter((n) => n !== 'value' && n !== 'now');
  const latest = latestValue(d.draft, d.observed, r.source_id ?? '', r.metric ?? '');
  const testValue = trial ?? latest?.value;
  const result = testValue !== undefined && !syntax ? evaluateExpression(r.expression ?? '', { value: testValue, now: Date.now() }) : null;
  const fires = result?.ok ? ruleFires(result.value) : false;
  const simple = simpleForm(r.expression ?? '');
  const surfaces = r.style?.surfaces ?? ['card'];
  const sev = r.style?.severity ?? 'info';
  const forMs = r.for_ms ?? 0;
  const allPanels = dash.pages.flatMap((p) => p.panels.map((x) => ({ value: x.id, label: `${p.title || p.id} / ${x.title || x.id}` })));

  const insert = (snippet: string) => {
    const el = exprRef.current;
    const cur = r.expression ?? '';
    const start = el?.selectionStart ?? cur.length;
    const end = el?.selectionEnd ?? cur.length;
    const before = cur.slice(0, start);
    const sep = before && !/\s$/.test(before) && !snippet.startsWith(')') ? ' ' : '';
    const next = before + sep + snippet + cur.slice(end);
    set((x) => (x.expression = next), 'expr');
    requestAnimationFrame(() => {
      if (!el) return;
      const caret = (before + sep + snippet).length - (snippet.endsWith('()') ? 1 : 0);
      el.focus();
      el.setSelectionRange(caret, caret);
    });
  };

  return (
    <div className="card card-body col gap-0 sticky-panel">
      <div className="row nowrap">
        <span className="page-icon sm">
          <Icon name="rules" size={18} />
        </span>
        <h3 className="grow truncate">{r.title || r.id}</h3>
        <Switch checked={r.enabled !== false} onChange={(v) => set((x) => (x.enabled = v), 'enabled')} label={r.enabled !== false ? t('rules.enabled') : t('rules.disabled')} />
        <Button
          size="sm"
          icon
          variant="ghost"
          title={t('common.duplicate')}
          onClick={() => {
            const id = uniqueId(r.id, dash.alert_rules.map((x) => x.id));
            d.update((b) => b.dashboard.alert_rules.splice(index + 1, 0, { ...structuredClone(r), id, title: `${r.title ?? r.id} 2` }));
            onSelect(id);
          }}
        >
          <Icon name="copy" size={16} />
        </Button>
        <Button
          size="sm"
          icon
          variant="ghost"
          title={t('common.delete')}
          onClick={() => {
            if (!confirm(t('rules.delete_confirm', { name: r.title || r.id }))) return;
            d.update((b) => (b.dashboard.alert_rules = b.dashboard.alert_rules.filter((x) => x.id !== r.id)));
            onSelect(null);
          }}
        >
          <Icon name="trash" size={16} />
        </Button>
      </div>
      {errs._ && <div className="notice danger mt-2">{errs._}</div>}

      <div className="form-grid two mt-3">
        <TextInput label={t('rules.name')} value={r.title} onChange={(v) => set((x) => (x.title = v), 'title')} error={errs.title} />
        <TextInput
          label="ID"
          value={r.id}
          error={errs.id}
          onChange={(v) => {
            const id = v.replace(/[^A-Za-z0-9_-]/g, '');
            if (!id || dash.alert_rules.some((x) => x.id === id)) return;
            set((x) => (x.id = id), 'id');
            onSelect(id);
          }}
        />
      </div>

      <Group title={t('rules.watch')}>
        <div className="form-grid two">
          <Select label={t('panel.source')} value={r.source_id ?? ''} options={[{ value: '', label: t('common.none') }, ...dash.sources.map((s) => ({ value: s.id, label: s.id }))]} onChange={(v) => set((x) => (x.source_id = v), 'src')} error={errs.source_id} />
          <TextInput label={t('panel.metric')} list={`rule-metrics-${r.id}`} value={r.metric} onChange={(v) => set((x) => (x.metric = v), 'metric')} error={errs.metric} placeholder="cpu.usage" />
          <datalist id={`rule-metrics-${r.id}`}>
            {metrics.map((m) => (
              <option key={m.name} value={m.name} />
            ))}
          </datalist>
        </div>
        {metrics.length > 0 && (
          <div className="chips mt-2">
            {metrics.slice(0, 12).map((m) => (
              <button key={m.name} type="button" className={`chip${m.name === r.metric ? ' on' : ''}`} onClick={() => set((x) => (x.metric = m.name), 'metric')}>
                {m.name}
              </button>
            ))}
          </div>
        )}
      </Group>

      <Group title={t('rules.condition')} hint={t('rules.condition_hint')}>
        <div className="row mb-3">
          <span className="mono">value</span>
          <Segmented size="sm" value={simple?.op ?? ('' as (typeof OPS)[number])} onChange={(op) => set((x) => (x.expression = `value ${op} ${simple?.n ?? 80}`), 'expr')} options={OPS.map((o) => ({ value: o, label: o }))} label={t('rules.operator')} />
          <div className="w-num">
            <NumberInput aria-label={t('rules.threshold')} value={simple?.n} onChange={(n) => n !== undefined && set((x) => (x.expression = `value ${simple?.op ?? '>'} ${n}`), 'expr')} placeholder="80" />
          </div>
          {latest?.unit && <span className="dim">{latest.unit}</span>}
        </div>
        <Field label={t('rules.expression')} error={errs.expression || (syntax ? t('rules.syntax_at', { msg: syntax.message, pos: syntax.offset + 1 }) : undefined)} hint={syntax ? undefined : unknownIds.length ? t('rules.unknown_ids', { list: unknownIds.join(', ') }) : t('rules.syntax_ok')}>
          <textarea ref={exprRef} rows={2} spellCheck={false} value={r.expression ?? ''} onChange={(e) => set((x) => (x.expression = e.target.value), 'expr')} className="input expr-input" />
        </Field>
        <div className="chips mt-2">
          {['value', 'now', '&&', '||', '!', '( )', ...EXPRESSION_FUNCTIONS.filter((f) => f !== 'convert').map((f) => `${f}()`)].map((s) => (
            <button key={s} type="button" className="chip" onClick={() => insert(s === '( )' ? '()' : s)}>
              {s}
            </button>
          ))}
        </div>
      </Group>

      <Group title={t('rules.preview')}>
        <div className={`eval${result?.ok ? (fires ? ' fire' : ' calm') : ''}`}>
          <Icon name={!result ? 'info' : !result.ok ? 'error' : fires ? 'warn' : 'ok'} size={22} />
          <div className="grow col gap-0">
            {testValue !== undefined ? (
              <span className="eval-value">{formatValue(testValue, latest?.unit ?? '', 2)}</span>
            ) : (
              <span className="muted">{t('rules.no_value')}</span>
            )}
            <span className="dim">
              {!result
                ? t('rules.preview_hint')
                : !result.ok
                  ? result.error
                  : fires
                    ? forMs > 0
                      ? t('rules.would_fire_for', { d: fmtDuration(forMs, t) })
                      : t('rules.would_fire')
                    : t('rules.would_not_fire')}
              {trial === undefined && latest && ` · ${latest.live ? t('rules.from_preview') : t('rules.from_samples')}`}
            </span>
          </div>
          <div className="w-num">
            <NumberInput label={t('rules.try_value')} allowEmpty value={trial} onChange={setTrial} placeholder={latest ? String(latest.value) : '42'} />
          </div>
        </div>
        {!latest && r.source_id && (
          <div className="dim mt-2">
            <a href={`#/sources/${encodeURIComponent(r.source_id)}`}>{t('rules.run_preview')}</a>
          </div>
        )}
      </Group>

      <Group title={t('rules.for')}>
        <div className="row">
          <Segmented size="sm" value={DURATIONS.includes(forMs) ? String(forMs) : 'custom'} onChange={(v) => v !== 'custom' && set((x) => (x.for_ms = Number(v)), 'for')} options={[...DURATIONS.map((ms) => ({ value: String(ms), label: fmtDuration(ms, t) })), { value: 'custom', label: t('common.custom') }]} />
          <div className="w-num">
            <NumberInput aria-label={t('rules.seconds_label')} min={0} max={604800} value={Math.round(forMs / 1000)} onChange={(v) => v !== undefined && set((x) => (x.for_ms = Math.round(v) * 1000), 'for')} error={errs.for_ms} />
          </div>
          <span className="dim">{t('rules.seconds_label')}</span>
        </div>
      </Group>

      <Group title={t('rules.presentation')}>
        <div className="col gap-md">
          <Segmented<Severity> value={sev} onChange={(v) => setStyle({ severity: v })} options={SEVERITIES.map((s) => ({ value: s, label: label('severity', s), icon: s === 'critical' ? 'error' : s === 'warning' ? 'warn' : 'info' }))} label={t('panel.severity')} />
          <div>
            <div className="dim mb-2">{t('rules.surfaces')}</div>
            <div className="chips">
              {SURFACES.map((s) => {
                const on = surfaces.includes(s);
                return (
                  <button key={s} type="button" className={`chip text${on ? ' on' : ''}`} onClick={() => setStyle({ surfaces: on ? (surfaces.length > 1 ? surfaces.filter((x) => x !== s) : surfaces) : [...surfaces, s] })}>
                    {on && '✓ '}
                    {t(`rules.surface.${s}`)}
                  </button>
                );
              })}
            </div>
            {errs['style.surfaces'] && <div className="err-text">{errs['style.surfaces']}</div>}
          </div>
          <Select label={t('rules.linked_panel')} value={r.style?.linked_panel_id ?? ''} options={[{ value: '', label: t('common.none') }, ...allPanels]} onChange={(v) => setStyle({ linked_panel_id: v })} error={errs['style.linked_panel_id']} hint={t('rules.linked_hint')} />
          <TextArea label={t('rules.message')} hint={t('rules.message_hint')} value={r.message ?? ''} onChange={(v) => set((x) => (x.message = v), 'msg')} rows={2} error={errs.message} />
        </div>
      </Group>
    </div>
  );
}
