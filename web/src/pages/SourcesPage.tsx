// Source wizard: connection settings, raw response probe rendered as a JSON
// tree, click-to-assign JSONPath mappings, and normalised preview via
// /dashboard/sources/test. Sources live in the dashboard draft (apply needed).
import { useCallback, useEffect, useMemo, useState } from 'react';
import * as api from '../api';
import { ApiError } from '../api';
import { useI18n, type DictKey } from '../i18n';
import { useDraft } from '../state/draft';
import { useToast } from '../state/toast';
import { errorText } from '../state/errors';
import { navigate } from '../state/router';
import { clone, defaultPlan, sourceFromPlan, syncPlan, uniqueId } from '../model/bundle';
import { sourceErrors } from '../model/errors';
import { pathForField, prefixMatch, type Segment } from '../model/jsonpath';
import { RESPONSE_KINDS, type Bundle, type EventMapping, type MetricMapping, type SourcePlan } from '../model/types';
import { Badge, Button, Card, Checkbox, Empty, Icon, KeyValueEditor, NumberInput, Select, TextArea, TextInput, type Option, PageHeader } from '../ui';
import { JsonTree } from '../ui/JsonTree';
import { DraftToolbar } from './editor/EditorPage';

// ---------------------------------------------------------------- rate limit
// Device limits sources/test and sources/probe to 5/min per token each.
const WINDOW_MS = 60000;
const LIMIT = 5;
const calls: Record<'test' | 'probe', number[]> = { test: [], probe: [] };
export function waitSeconds(kind: 'test' | 'probe', now = Date.now()): number {
  calls[kind] = calls[kind].filter((ts) => now - ts < WINDOW_MS);
  if (calls[kind].length < LIMIT) return 0;
  return Math.ceil((WINDOW_MS - (now - calls[kind][0])) / 1000);
}
export function noteCall(kind: 'test' | 'probe') {
  calls[kind].push(Date.now());
}

type ActiveField = { list: 'metrics' | 'events'; index: number; field: string; label?: number } | null;

const METRIC_PATH_FIELDS: { key: keyof MetricMapping; label: DictKey }[] = [
  { key: 'items_path', label: 'map.items_path' },
  { key: 'value_path', label: 'map.value_path' },
  { key: 'name_path', label: 'map.name_path' },
  { key: 'unit_path', label: 'map.unit_path' },
  { key: 'timestamp_path', label: 'map.timestamp_path' },
];
const EVENT_PATH_FIELDS: { key: keyof EventMapping; label: DictKey }[] = [
  { key: 'items_path', label: 'map.items_path' },
  { key: 'id_path', label: 'map.id_path' },
  { key: 'severity_path', label: 'map.severity_path' },
  { key: 'state_path', label: 'map.state_path' },
  { key: 'title_path', label: 'map.title_path' },
  { key: 'message_path', label: 'map.message_path' },
  { key: 'timestamp_path', label: 'map.timestamp_path' },
];

function variableDefaults(b: Bundle): Record<string, string> {
  return Object.fromEntries(b.dashboard.variables.map((v) => [v.name, v.default_value ?? '']));
}

function PathInput({ label, value, active, onFocus, onChange, error }: { label: string; value: string | undefined; active: boolean; onFocus: () => void; onChange: (v: string) => void; error?: string }) {
  return (
    <div className={`path-input${active ? ' active' : ''}`} onFocusCapture={onFocus}>
      <TextInput label={label} value={value} onChange={onChange} placeholder="$." spellCheck={false} error={error} />
    </div>
  );
}

export function SourcesPage({ sourceId }: { sourceId: string }) {
  const { t } = useI18n();
  const d = useDraft();
  const sources = d.draft.dashboard.sources;
  const selected = sources.find((s) => s.id === sourceId) ? sourceId : sources[0]?.id ?? '';

  const addSource = () => {
    const id = uniqueId('source', sources.map((s) => s.id));
    d.update((b) => {
      const s = { id, kind: 'http' as const, enabled: true, url: 'https://', refresh_ms: 60000, timeout_ms: 10000, max_response_bytes: 65536, tls_verify: true, auth_token_ref: '', headers: {} };
      b.dashboard.sources.push(s);
      b.source_plans.push({ ...defaultPlan(s), response: 'generic_json', metrics: [] });
    });
    navigate('sources', id);
  };

  return (
    <div className="page wide">
      <PageHeader icon="db" title={t('src.title')} subtitle={t('src.subtitle')} />
      <div className="card row" style={{ padding: '8px 12px', marginBottom: 16 }}>
        <DraftToolbar compact />
      </div>
      <div className="src-layout">
        <div className="card src-list" style={{ padding: 6 }}>
          {sources.map((s) => {
            const used = d.draft.dashboard.pages.reduce((n, p) => n + p.panels.filter((x) => x.source_id === s.id).length, 0);
            const errs = Object.keys(sourceErrors(d.draft, d.errors, s.id)).length;
            return (
              <div key={s.id} className={`page-item${s.id === selected ? ' on' : ''}`} role="button" tabIndex={0} onClick={() => navigate('sources', s.id)} onKeyDown={(e) => e.key === 'Enter' && navigate('sources', s.id)}>
                <span className="grow">
                  {s.id}
                  <div className="dim">{t('src.used_by', { n: used })}</div>
                </span>
                {errs > 0 && <Badge tone="danger">{errs}</Badge>}
                {s.enabled === false && <Badge>off</Badge>}
              </div>
            );
          })}
          {!sources.length && <Empty>{t('src.empty')}</Empty>}
          <Button size="sm" onClick={addSource} style={{ margin: 6 }}>
            <Icon name="plus" /> {t('src.add')}
          </Button>
        </div>
        {selected ? <SourceEditor key={selected} sourceId={selected} /> : <div />}
      </div>
    </div>
  );
}

function SourceEditor({ sourceId }: { sourceId: string }) {
  const { t } = useI18n();
  const toast = useToast();
  const d = useDraft();
  const source = d.draft.dashboard.sources.find((s) => s.id === sourceId)!;
  const plan: SourcePlan = useMemo(() => syncPlan(d.draft.source_plans.find((p) => p.id === sourceId) ?? defaultPlan(source), source), [d.draft.source_plans, source, sourceId]);
  const errs = useMemo(() => sourceErrors(d.draft, d.errors, sourceId), [d.draft, d.errors, sourceId]);
  const [creds, setCreds] = useState<api.CredentialMeta[]>([]);
  const [active, setActive] = useState<ActiveField>(null);
  const [probe, setProbe] = useState<api.ProbeResult | null>(null);
  const [preview, setPreview] = useState<api.SourceTestResult | null>(null);
  const [busy, setBusy] = useState<'probe' | 'test' | null>(null);
  const [, tick] = useState(0);

  useEffect(() => {
    api.listCredentials().then((r) => setCreds(r.credentials), () => undefined);
  }, []);
  useEffect(() => {
    const id = setInterval(() => tick((n) => n + 1), 1000);
    return () => clearInterval(id);
  }, []);

  /** Mutate the plan; the dashboard source mirrors its connection fields. */
  const setPlan = useCallback(
    (fn: (p: SourcePlan) => void, key = 'x') =>
      d.update((b) => {
        const s = b.dashboard.sources.find((x) => x.id === sourceId);
        if (!s) return;
        let i = b.source_plans.findIndex((x) => x.id === sourceId);
        if (i < 0) {
          b.source_plans.push(defaultPlan(s));
          i = b.source_plans.length - 1;
        }
        const p = syncPlan(b.source_plans[i], s);
        fn(p);
        b.source_plans[i] = p;
        Object.assign(s, sourceFromPlan(p));
        if (p.id !== sourceId) {
          for (const pg of b.dashboard.pages) for (const x of pg.panels) if (x.source_id === sourceId) x.source_id = p.id;
          for (const r of b.dashboard.alert_rules) if (r.source_id === sourceId) r.source_id = p.id;
        }
      }, `src:${sourceId}:${key}`),
    [d, sourceId],
  );

  const credOptions: Option[] = [
    { value: '', label: t('src.no_auth') },
    ...creds.map((c) => ({ value: c.ref, label: c.ref })),
    ...(plan.auth_token_ref && !creds.some((c) => c.ref === plan.auth_token_ref) ? [{ value: plan.auth_token_ref, label: t('src.credential_missing', { ref: plan.auth_token_ref }) }] : []),
  ];
  const variables = variableDefaults(d.draft);

  const runProbe = async () => {
    const w = waitSeconds('probe');
    if (w) return toast('warn', t('src.preview_wait', { s: w }));
    noteCall('probe');
    setBusy('probe');
    try {
      setProbe(await api.probeSource(clone(plan), variables));
    } catch (e) {
      if (e instanceof ApiError && e.status === 429) calls.probe = Array(LIMIT).fill(Date.now() - WINDOW_MS + (e.retryAfter ?? 60) * 1000);
      toast('error', errorText(e, t));
    } finally {
      setBusy(null);
    }
  };
  const runTest = async () => {
    const w = waitSeconds('test');
    if (w) return toast('warn', t('src.preview_wait', { s: w }));
    noteCall('test');
    setBusy('test');
    try {
      const r = await api.testSource(clone(plan), variables);
      setPreview(r);
      d.recordObserved(sourceId, r.metrics.map((m) => ({ metric: m.metric, unit: m.unit, value: m.value, timestamp_ms: m.timestamp_ms, labels: m.labels })));
    } catch (e) {
      if (e instanceof ApiError && e.status === 429) calls.test = Array(LIMIT).fill(Date.now() - WINDOW_MS + (e.retryAfter ?? 60) * 1000);
      toast('error', errorText(e, t));
    } finally {
      setBusy(null);
    }
  };

  const onPick = (segs: Segment[]) => {
    if (!active) return;
    setPlan((p) => {
      if (active.list === 'metrics') {
        const m = (p.metrics ??= [])[active.index];
        if (!m) return;
        if (active.label !== undefined) {
          const l = m.labels?.[active.label];
          if (l) l.path = pathForField('path', segs, m.items_path);
        } else {
          (m as Record<string, unknown>)[active.field] = pathForField(active.field, segs, m.items_path);
          if (active.field === 'name_path') delete m.name;
          if (active.field === 'unit_path') delete m.unit;
        }
      } else {
        const ev = (p.events ??= [])[active.index];
        if (ev) (ev as Record<string, unknown>)[active.field] = pathForField(active.field, segs, ev.items_path);
      }
    });
    // Advance focus helpfully: after items_path pick the value path.
    if (active.list === 'metrics' && active.field === 'items_path') setActive({ ...active, field: 'value_path' });
  };

  const isMatch = useCallback(
    (segs: Segment[]) => {
      if (!active) return false;
      const m = active.list === 'metrics' ? plan.metrics?.[active.index] : plan.events?.[active.index];
      if (!m) return false;
      const items = m.items_path;
      const path = active.label !== undefined ? (m as MetricMapping).labels?.[active.label]?.path : ((m as Record<string, unknown>)[active.field] as string | undefined);
      if (!path) return false;
      if (active.field === 'items_path') return prefixMatch(path, segs) === segs.length;
      const n = items && items !== '$' ? prefixMatch(items, segs) : 0;
      if (n < 0) return false;
      return prefixMatch(path, segs.slice(n)) === segs.length - n;
    },
    [active, plan],
  );

  const deleteSource = () => {
    if (!confirm(t('src.delete_confirm', { id: sourceId }))) return;
    d.update((b) => {
      b.dashboard.sources = b.dashboard.sources.filter((s) => s.id !== sourceId);
      b.source_plans = b.source_plans.filter((p) => p.id !== sourceId);
    });
    navigate('sources');
  };

  const isJsonResponse = ['generic_json', 'standard_json', 'metrics_json', 'events_json'].includes(plan.response ?? 'standard_json');
  const probeWait = waitSeconds('probe');
  const testWait = waitSeconds('test');

  return (
    <div className="col" style={{ gap: 14 }}>
      <Card
        title={source.id}
        actions={
          <Button size="sm" variant="danger" onClick={deleteSource}>
            {t('common.delete')}
          </Button>
        }
      >
        {errs._ && <div className="notice danger" style={{ marginBottom: 10 }}>{errs._}</div>}
        <div className="form-grid">
          <TextInput
            label={t('src.id')}
            value={plan.id}
            error={errs.id}
            onChange={(v) => {
              const id = v.replace(/[^A-Za-z0-9_-]/g, '');
              if (!id || d.draft.dashboard.sources.some((s) => s.id === id)) return;
              setPlan((p) => (p.id = id), 'id');
              navigate('sources', id);
            }}
          />
          <Select label={t('src.kind')} value={plan.kind} options={[{ value: 'http', label: t('src.kind.http') }, { value: 'static', label: t('src.kind.static') }]} onChange={(v) => setPlan((p) => (p.kind = v as SourcePlan['kind']))} />
          <Checkbox label={t('src.enabled')} checked={plan.enabled !== false} onChange={(v) => setPlan((p) => (p.enabled = v))} />
          {plan.kind === 'http' ? (
            <>
              <TextInput wide label={t('src.url')} hint={t('src.url_hint')} value={plan.url} error={errs.url} onChange={(v) => setPlan((p) => (p.url = v), 'url')} inputMode="url" spellCheck={false} />
              <Select label={t('src.method')} value={plan.method ?? 'GET'} options={['GET', 'POST'].map((m) => ({ value: m, label: m }))} onChange={(v) => setPlan((p) => (p.method = v))} error={errs.method} />
              <NumberInput label={t('src.refresh')} min={1} value={Math.round((plan.refresh_ms ?? 60000) / 1000)} onChange={(v) => v !== undefined && setPlan((p) => (p.refresh_ms = Math.round(v * 1000)), 'refresh')} error={errs.refresh_ms} />
              <Select label={t('src.auth')} value={plan.auth_token_ref ?? ''} options={credOptions} onChange={(v) => setPlan((p) => (p.auth_token_ref = v))} error={errs.auth_token_ref} hint={<a href="#/credentials">{t('nav.credentials')} →</a>} />
              {plan.method === 'POST' && <TextArea label={t('src.body')} value={plan.body ?? ''} onChange={(v) => setPlan((p) => (p.body = v), 'body')} rows={3} error={errs.body} />}
            </>
          ) : (
            <TextArea label={t('src.static_body')} value={plan.static_body ?? ''} onChange={(v) => setPlan((p) => (p.static_body = v), 'static')} rows={5} error={errs.static_body} />
          )}
          <Select label={t('src.response')} value={plan.response ?? 'standard_json'} options={RESPONSE_KINDS.map((r) => ({ value: r, label: t(`src.response.${r}` as DictKey) }))} onChange={(v) => setPlan((p) => (p.response = v as SourcePlan['response']))} error={errs.response} />
          {plan.response === 'text' && <TextInput label={t('src.text_path')} value={plan.text_path} onChange={(v) => setPlan((p) => (p.text_path = v), 'tp')} />}
        </div>
        {plan.kind === 'http' && (
          <details style={{ marginTop: 12 }}>
            <summary className="muted">{t('common.advanced')}</summary>
            <div className="form-grid" style={{ marginTop: 10 }}>
              <NumberInput label={t('src.timeout')} min={100} max={120000} value={plan.timeout_ms ?? 10000} onChange={(v) => v !== undefined && setPlan((p) => (p.timeout_ms = v), 'to')} error={errs.timeout_ms} />
              <NumberInput label={t('src.max_bytes')} min={1024} max={262144} value={plan.max_response_bytes ?? 65536} onChange={(v) => v !== undefined && setPlan((p) => (p.max_response_bytes = v), 'mb')} error={errs.max_response_bytes} />
              <NumberInput label={t('src.stale_after')} min={0} value={plan.stale_after_ms ?? 300000} onChange={(v) => v !== undefined && setPlan((p) => (p.stale_after_ms = v), 'st')} />
              <TextInput label={t('src.reset_at_path')} value={plan.reset_at_path} onChange={(v) => setPlan((p) => (p.reset_at_path = v), 'ra')} />
              <Checkbox label={t('src.tls')} checked={plan.tls_verify !== false} onChange={(v) => setPlan((p) => (p.tls_verify = v))} />
              <Checkbox label={t('src.conditional_get')} checked={plan.conditional_get !== false} onChange={(v) => setPlan((p) => (p.conditional_get = v))} />
              <div className="wide">
                <h4 style={{ marginBottom: 6 }}>{t('src.headers')}</h4>
                <KeyValueEditor value={plan.headers} onChange={(v) => setPlan((p) => (p.headers = v), 'headers')} keyLabel={t('src.header_name')} valueLabel={t('src.header_value')} addLabel={t('src.add_header')} />
                {errs.headers && <div className="err-text">{errs.headers}</div>}
              </div>
              <div className="wide">
                <h4 style={{ marginBottom: 6 }}>{t('src.secret_headers')}</h4>
                <KeyValueEditor
                  value={plan.header_secret_refs}
                  onChange={(v) => setPlan((p) => (p.header_secret_refs = v), 'hsr')}
                  keyLabel={t('src.header_name')}
                  valueLabel={t('cred.ref')}
                  addLabel={t('src.add_header')}
                  renderValue={(v, setV) => (
                    <select className="input" value={v} onChange={(e) => setV(e.target.value)} aria-label={t('cred.ref')}>
                      <option value="">—</option>
                      {creds.map((c) => (
                        <option key={c.ref} value={c.ref}>
                          {c.ref}
                        </option>
                      ))}
                      {v && !creds.some((c) => c.ref === v) && <option value={v}>{v}</option>}
                    </select>
                  )}
                />
                {errs.header_secret_refs && <div className="err-text">{errs.header_secret_refs}</div>}
              </div>
            </div>
          </details>
        )}
      </Card>

      <div className="src-split">
        <div className="col" style={{ gap: 14 }}>
          {isJsonResponse && (
            <Card
              title={t('src.mappings')}
              actions={
                <Button size="sm" onClick={() => setPlan((p) => (p.metrics = [...(p.metrics ?? []), { name: '', items_path: '$', value_path: '', labels: [] }]))}>
                  <Icon name="plus" /> {t('src.add_mapping')}
                </Button>
              }
            >
              {!plan.metrics?.length && <div className="dim">{plan.response === 'generic_json' ? t('common.empty') : t('src.response.' + (plan.response ?? 'standard_json') as DictKey)}</div>}
              <div className="col">
                {(plan.metrics ?? []).map((m, i) => (
                  <div key={i} className="subcard">
                    <div className="form-grid two">
                      <TextInput label={t('map.name')} value={m.name} onChange={(v) => setPlan((p) => { p.metrics![i].name = v || undefined; if (v) delete p.metrics![i].name_path; }, `m${i}name`)} />
                      {METRIC_PATH_FIELDS.map((f) => (
                        <PathInput
                          key={f.key}
                          label={t(f.label)}
                          value={m[f.key] as string | undefined}
                          active={active?.list === 'metrics' && active.index === i && active.field === f.key && active.label === undefined}
                          onFocus={() => setActive({ list: 'metrics', index: i, field: f.key })}
                          onChange={(v) => setPlan((p) => ((p.metrics![i] as Record<string, unknown>)[f.key] = v || undefined), `m${i}${f.key}`)}
                        />
                      ))}
                      <TextInput label={t('map.unit')} value={m.unit} onChange={(v) => setPlan((p) => (p.metrics![i].unit = v || undefined), `m${i}unit`)} />
                      <TextInput label={t('map.target_unit')} value={m.target_unit} onChange={(v) => setPlan((p) => (p.metrics![i].target_unit = v || undefined), `m${i}tu`)} />
                      <TextInput label={t('map.expression')} value={m.expression} onChange={(v) => setPlan((p) => (p.metrics![i].expression = v || undefined), `m${i}ex`)} />
                    </div>
                    <div className="section" style={{ marginTop: 8, paddingTop: 8 }}>
                      <h4>{t('src.labels')}</h4>
                      {(m.labels ?? []).map((l, li) => (
                        <div key={li} className="kv-row" style={{ gridTemplateColumns: '1fr 1fr 1fr auto', marginBottom: 6 }}>
                          <input className="input" placeholder={t('map.label_name')} value={l.name} onChange={(e) => setPlan((p) => (p.metrics![i].labels![li].name = e.target.value), `m${i}l${li}n`)} />
                          <div className={`path-input${active?.label === li && active.index === i ? ' active' : ''}`}>
                            <input className="input" placeholder={t('map.label_path')} value={l.path ?? ''} onFocus={() => setActive({ list: 'metrics', index: i, field: 'path', label: li })} onChange={(e) => setPlan((p) => (p.metrics![i].labels![li].path = e.target.value || undefined), `m${i}l${li}p`)} />
                          </div>
                          <input className="input" placeholder={t('map.label_value')} value={l.value ?? ''} onChange={(e) => setPlan((p) => (p.metrics![i].labels![li].value = e.target.value || undefined), `m${i}l${li}v`)} />
                          <Button icon onClick={() => setPlan((p) => p.metrics![i].labels!.splice(li, 1))} aria-label={t('common.delete')}>
                            <Icon name="x" />
                          </Button>
                        </div>
                      ))}
                      <div className="row">
                        <Button size="sm" onClick={() => setPlan((p) => (p.metrics![i].labels = [...(p.metrics![i].labels ?? []), { name: 'label', path: '' }]))}>
                          <Icon name="plus" /> {t('src.add_label')}
                        </Button>
                        <span className="spacer" />
                        <Button size="sm" variant="danger" onClick={() => setPlan((p) => p.metrics!.splice(i, 1))}>
                          {t('common.delete')}
                        </Button>
                      </div>
                    </div>
                  </div>
                ))}
              </div>
            </Card>
          )}
          {isJsonResponse && (
            <Card
              title={t('src.event_mappings')}
              actions={
                <Button size="sm" onClick={() => setPlan((p) => (p.events = [...(p.events ?? []), { items_path: '$', title_path: '', severity_map: {}, clear_states: [] }]))}>
                  <Icon name="plus" /> {t('src.add_event_mapping')}
                </Button>
              }
            >
              <div className="col">
                {(plan.events ?? []).map((ev, i) => (
                  <div key={i} className="subcard">
                    <div className="form-grid two">
                      {EVENT_PATH_FIELDS.map((f) => (
                        <PathInput
                          key={f.key}
                          label={t(f.label)}
                          value={ev[f.key] as string | undefined}
                          active={active?.list === 'events' && active.index === i && active.field === f.key}
                          onFocus={() => setActive({ list: 'events', index: i, field: f.key })}
                          onChange={(v) => setPlan((p) => ((p.events![i] as Record<string, unknown>)[f.key] = v || undefined), `e${i}${f.key}`)}
                        />
                      ))}
                    </div>
                    <div className="row" style={{ justifyContent: 'flex-end', marginTop: 6 }}>
                      <Button size="sm" variant="danger" onClick={() => setPlan((p) => p.events!.splice(i, 1))}>
                        {t('common.delete')}
                      </Button>
                    </div>
                  </div>
                ))}
              </div>
            </Card>
          )}
        </div>

        <div className="col" style={{ gap: 14 }}>
          <Card
            title={t('src.probe_title')}
            actions={
              <Button size="sm" variant="primary" loading={busy === 'probe'} disabled={probeWait > 0} onClick={() => void runProbe()}>
                <Icon name="fetch" /> {probeWait ? `${probeWait}s` : t('src.fetch')}
              </Button>
            }
          >
            {probe ? (
              <div className="col">
                <div className="row dim">
                  <Badge tone={probe.status_code >= 200 && probe.status_code < 300 ? 'ok' : 'danger'}>{t('src.status', { code: probe.status_code, ms: probe.latency_ms })}</Badge>
                  <span className="mono">{probe.content_type}</span>
                  <span>{probe.body_bytes} B</span>
                  {probe.masked_values > 0 && <Badge tone="warn">{t('src.probe_masked', { n: probe.masked_values })}</Badge>}
                  {probe.truncated && <Badge tone="warn">{t('src.probe_truncated')}</Badge>}
                </div>
                {probe.error && <div className="notice danger">{probe.error}</div>}
                {active && <div className="notice">{t('src.active_field', { field: active.label !== undefined ? `labels[${active.label}].path` : active.field })} · {t('src.probe_hint')}</div>}
                {probe.format === 'json' && (
                  <div className="jt-box">
                    <JsonTree value={probe.json} onPick={onPick} isMatch={isMatch} />
                  </div>
                )}
                {probe.format === 'text' && (
                  <>
                    <div className="dim">{t('src.probe_not_json')}</div>
                    <pre className="jt-box">{probe.text}</pre>
                  </>
                )}
                {probe.format === 'binary' && <div className="dim">{t('src.probe_binary')}</div>}
              </div>
            ) : (
              <div className="dim">{t('src.probe_hint')}</div>
            )}
          </Card>
          <Card
            title={t('src.preview')}
            actions={
              <Button size="sm" loading={busy === 'test'} disabled={testWait > 0} onClick={() => void runTest()}>
                <Icon name="play" /> {testWait ? `${testWait}s` : t('src.run_preview')}
              </Button>
            }
          >
            <div className="dim" style={{ marginBottom: 8 }}>{t('src.preview_hint')}</div>
            {preview && (
              <div className="col">
                <div className="row">
                  <Badge tone={preview.status_code >= 200 && preview.status_code < 300 ? 'ok' : 'danger'}>{t('src.status', { code: preview.status_code, ms: preview.latency_ms })}</Badge>
                  {preview.error && <span className="err-text">{preview.error}</span>}
                </div>
                {preview.metrics.length ? (
                  <div className="scroll-x">
                    <table className="table">
                      <thead>
                        <tr>
                          <th>{t('panel.metric')}</th>
                          <th>value</th>
                          <th>{t('panel.unit')}</th>
                          <th>{t('src.labels')}</th>
                        </tr>
                      </thead>
                      <tbody>
                        {preview.metrics.map((m, i) => (
                          <tr key={i}>
                            <td className="mono">{m.metric}</td>
                            <td className="mono">{m.value}</td>
                            <td>{m.unit}</td>
                            <td className="dim">{Object.entries(m.labels).map(([k, v]) => `${k}=${v}`).join(', ')}</td>
                          </tr>
                        ))}
                      </tbody>
                    </table>
                  </div>
                ) : (
                  <div className="dim">{t('src.preview_empty')}</div>
                )}
                {preview.events.length > 0 && (
                  <ul style={{ margin: 0, paddingLeft: 18 }}>
                    {preview.events.map((e, i) => (
                      <li key={i}>
                        <Badge tone={e.severity === 'critical' ? 'danger' : e.severity === 'warning' ? 'warn' : undefined}>{e.severity}</Badge> {e.title} <span className="dim">{e.message}</span>
                      </li>
                    ))}
                  </ul>
                )}
                {preview.assets.map((a, i) => (
                  <div key={i} className="dim">
                    {a.kind} · {a.media_type} · {a.size_bytes} B
                  </div>
                ))}
              </div>
            )}
          </Card>
        </div>
      </div>
    </div>
  );
}
