// First-run onboarding: pick a template → connect its data sources (probe) →
// map panels to metrics → apply. Works on a local copy of the bundle and
// only touches the draft/device at the last step.
import { useEffect, useState } from 'react';
import * as api from '../api';
import { useI18n, useEnumLabel } from '../i18n';
import { useDraft } from '../state/draft';
import { useToast } from '../state/toast';
import { errorText } from '../state/errors';
import { navigate } from '../state/router';
import { clone, defaultPlan, emptyBundle, replaceWithTemplate, sourceFromPlan, syncPlan, uniqueId } from '../model/bundle';
import { toJsonPath, type Segment } from '../model/jsonpath';
import { metricsForSource, type ObservedMetrics } from '../model/metrics';
import type { Bundle, Panel, SourcePlan } from '../model/types';
import { Badge, Button, EmptyState, Icon, Select, Skeleton, TextInput } from '../ui';
import { JsonTree } from '../ui/JsonTree';
import { ScreenThumb, libraryEntries, type TemplateEntry } from './TemplatesPage';
import { noteCall, waitSeconds } from './SourcesPage';

export const WELCOME_SKIP_KEY = 'eego_welcome_skipped';
type Step = 0 | 1 | 2 | 3;

function blankEntry(title: string): TemplateEntry {
  return { id: 'blank', kind: 'layout', title, bundle: emptyBundle() };
}

const dataPanels = (b: Bundle): Panel[] => b.dashboard.pages.flatMap((p) => p.panels.filter((x) => x.visible !== false && (x.kind === 'card' || x.kind === 'chart' || x.kind === 'table')));

export function WelcomePage() {
  const { t } = useI18n();
  const label = useEnumLabel();
  const toast = useToast();
  const d = useDraft();
  const [step, setStep] = useState<Step>(0);
  const [entries, setEntries] = useState<TemplateEntry[] | null>(null);
  const [loadErr, setLoadErr] = useState<string | null>(null);
  const [picked, setPicked] = useState<TemplateEntry | null>(null);
  const [wb, setWb] = useState<Bundle | null>(null);
  const [probes, setProbes] = useState<Record<string, api.ProbeResult | string>>({});
  const [busy, setBusy] = useState<string | null>(null);
  const [observed, setObserved] = useState<ObservedMetrics>({});
  const [focusPanel, setFocusPanel] = useState<string | null>(null);
  const [creds, setCreds] = useState<api.CredentialMeta[]>([]);

  useEffect(() => {
    api.getTemplates().then(
      (lib) => setEntries(libraryEntries(lib).filter((e) => e.kind !== 'panel')),
      (e) => {
        setEntries([]);
        setLoadErr(errorText(e, t));
      },
    );
    api.listCredentials().then((r) => setCreds(r.credentials), () => undefined);
  }, [t]);

  const skip = () => {
    try {
      sessionStorage.setItem(WELCOME_SKIP_KEY, '1');
    } catch {
      /* ignore */
    }
    navigate('editor');
  };

  const choose = (e: TemplateEntry) => {
    setPicked(e);
    const b = replaceWithTemplate(d.draft, e.bundle);
    for (const s of b.dashboard.sources) if (!b.source_plans.some((p) => p.id === s.id)) b.source_plans.push(defaultPlan(s));
    setWb(b);
    setProbes({});
    setObserved({});
  };

  const plans: SourcePlan[] = wb ? wb.dashboard.sources.map((s) => syncPlan(wb.source_plans.find((p) => p.id === s.id) ?? defaultPlan(s), s)) : [];
  const setPlan = (id: string, fn: (p: SourcePlan) => void) =>
    setWb((b) => {
      if (!b) return b;
      const n = clone(b);
      const s = n.dashboard.sources.find((x) => x.id === id);
      if (!s) return b;
      let i = n.source_plans.findIndex((x) => x.id === id);
      if (i < 0) {
        n.source_plans.push(defaultPlan(s));
        i = n.source_plans.length - 1;
      }
      const p = syncPlan(n.source_plans[i], s);
      fn(p);
      n.source_plans[i] = p;
      Object.assign(s, sourceFromPlan(p));
      return n;
    });
  const addSource = () =>
    setWb((b) => {
      if (!b) return b;
      const n = clone(b);
      const id = uniqueId('source', n.dashboard.sources.map((s) => s.id));
      const s = { id, kind: 'http' as const, enabled: true, url: 'https://', refresh_ms: 60000, timeout_ms: 10000, max_response_bytes: 65536, tls_verify: true, auth_token_ref: '', headers: {} };
      n.dashboard.sources.push(s);
      n.source_plans.push({ ...defaultPlan(s), response: 'generic_json', metrics: [] });
      for (const p of dataPanels(n)) if (!p.source_id) p.source_id = id;
      return n;
    });
  const vars = wb ? Object.fromEntries(wb.dashboard.variables.map((v) => [v.name, v.default_value ?? ''])) : {};

  const probe = async (p: SourcePlan) => {
    const w = waitSeconds('probe');
    if (w) return toast('warn', t('src.preview_wait', { s: w }));
    noteCall('probe');
    setBusy(`probe:${p.id}`);
    try {
      const r = await api.probeSource(clone(p), vars);
      setProbes((x) => ({ ...x, [p.id]: r }));
    } catch (e) {
      setProbes((x) => ({ ...x, [p.id]: errorText(e, t) }));
    } finally {
      setBusy(null);
    }
  };
  const testAll = async () => {
    if (!wb) return;
    setBusy('test');
    try {
      for (const p of plans) {
        if (p.kind !== 'http' && p.kind !== 'static') continue;
        const w = waitSeconds('test');
        if (w) {
          toast('warn', t('src.preview_wait', { s: w }));
          break;
        }
        noteCall('test');
        try {
          const r = await api.testSource(clone(p), vars);
          const list = r.metrics.map((m) => ({ metric: m.metric, unit: m.unit, value: m.value, timestamp_ms: m.timestamp_ms, labels: m.labels }));
          setObserved((o) => ({ ...o, [p.id]: list }));
          d.recordObserved(p.id, list);
        } catch (e) {
          toast('error', `${p.id}: ${errorText(e, t)}`);
        }
      }
    } finally {
      setBusy(null);
    }
  };
  // Map metrics automatically when entering step 3.
  useEffect(() => {
    if (step === 2 && wb && !Object.keys(observed).length && wb.dashboard.sources.length) void testAll();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [step]);

  const setPanel = (id: string, fn: (p: Panel) => void) =>
    setWb((b) => {
      if (!b) return b;
      const n = clone(b);
      for (const pg of n.dashboard.pages) for (const p of pg.panels) if (p.id === id) fn(p);
      return n;
    });

  /** Click a JSON value in the probe → mapping for the focused panel. */
  const pickJson = (sourceId: string, segs: Segment[], value: unknown) => {
    if (typeof value !== 'number' || !focusPanel) {
      if (!focusPanel) toast('info', t('wz.pick_panel_first'));
      else toast('warn', t('wz.pick_number'));
      return;
    }
    const name = segs.filter((s) => typeof s === 'string').join('.') || 'value';
    setPlan(sourceId, (p) => {
      p.metrics = (p.metrics ?? []).filter((m) => m.name !== name);
      p.metrics.push({ name, items_path: '$', value_path: toJsonPath(segs), labels: [] });
    });
    setPanel(focusPanel, (p) => {
      p.source_id = sourceId;
      p.metric = name;
    });
    setObserved((o) => ({ ...o, [sourceId]: [...(o[sourceId] ?? []).filter((m) => m.metric !== name), { metric: name, value }] }));
  };

  const finish = async (applyNow: boolean) => {
    if (!wb) return;
    try {
      sessionStorage.setItem(WELCOME_SKIP_KEY, '1');
    } catch {
      /* ignore */
    }
    for (const [id, list] of Object.entries(observed)) d.recordObserved(id, list);
    if (!applyNow) {
      d.replace(wb);
      navigate('editor');
      return;
    }
    const ok = await d.apply(wb);
    navigate('editor');
    if (!ok) toast('warn', t('wz.apply_failed'));
  };

  const steps = [t('wz.step.template'), t('wz.step.source'), t('wz.step.map'), t('wz.step.apply')];
  const panels = wb ? dataPanels(wb) : [];
  const mapped = panels.filter((p) => p.source_id && p.metric).length;
  const firstPage = wb?.dashboard.pages[0];

  return (
    <div className="wizard">
      <div className="wz-hero">
        <div className="empty-icon" style={{ margin: '0 auto 12px' }}>
          <Icon name="wand" size={26} />
        </div>
        <h1>{t('wz.title')}</h1>
        <p>{t('wz.subtitle')}</p>
      </div>
      <ol className="steps" style={{ padding: 0, listStyle: 'none' }}>
        {steps.map((s, i) => (
          <li key={s} className={`step${i === step ? ' on' : i < step ? ' done' : ''}`}>
            <span className="step-num">{i < step ? <Icon name="check" size={16} strokeWidth={2.5} /> : i + 1}</span>
            <span>{s}</span>
          </li>
        ))}
      </ol>

      {step === 0 && (
        <div className="col" style={{ gap: 14 }}>
          {loadErr && <div className="notice warn">{loadErr}</div>}
          <div className="tpl-grid">
            {[blankEntry(t('wz.blank')), ...(entries ?? [])].map((e) => {
              const page = e.bundle.dashboard.pages[0];
              const on = picked?.id === e.id && picked.kind === e.kind;
              return (
                <button key={`${e.kind}:${e.id}`} type="button" className={`card tpl-card${on ? ' picked' : ''}`} style={{ textAlign: 'left', padding: 0, cursor: 'pointer' }} onClick={() => choose(e)}>
                  <div className="tpl-thumb">{page && page.panels.length ? <ScreenThumb page={page} bundle={e.bundle} width={150} /> : <div className="thumb" style={{ width: 150, height: 209, display: 'grid', placeItems: 'center', color: '#999' }}><Icon name="plus" size={28} /></div>}</div>
                  <div className="tpl-meta">
                    <div className="row nowrap">
                      <strong className="grow truncate">{e.title}</strong>
                      {on && <Badge tone="accent">✓</Badge>}
                    </div>
                    {e.description && <div className="dim">{e.description}</div>}
                    <div className="dim">{t('wz.tpl_meta', { p: e.bundle.dashboard.pages.length, n: e.bundle.dashboard.pages.reduce((n, p) => n + p.panels.length, 0), s: e.bundle.dashboard.sources.length })}</div>
                  </div>
                </button>
              );
            })}
            {!entries && Array.from({ length: 4 }, (_, i) => <Skeleton key={i} h={320} r={10} />)}
          </div>
        </div>
      )}

      {step === 1 && wb && (
        <div className="col" style={{ gap: 14 }}>
          {!plans.length && (
            <div className="card">
              <EmptyState icon="db" title={t('wz.no_sources')} action={<Button onClick={addSource}><Icon name="plus" size={16} /> {t('src.add')}</Button>}>
                {t('wz.no_sources_desc')}
              </EmptyState>
            </div>
          )}
          {plans.map((p) => {
            const pr = probes[p.id];
            const ok = pr && typeof pr !== 'string' && pr.status_code >= 200 && pr.status_code < 300;
            return (
              <div key={p.id} className="card">
                <div className="card-head">
                  <Icon name="db" />
                  <h3 className="grow">{p.id}</h3>
                  {pr && (typeof pr === 'string' ? <Badge tone="danger">{t('wz.probe_failed')}</Badge> : <Badge tone={ok ? 'ok' : 'danger'} dot>{t('src.status', { code: pr.status_code, ms: pr.latency_ms })}</Badge>)}
                </div>
                <div className="card-body col" style={{ gap: 12 }}>
                  {p.kind === 'http' ? (
                    <div className="form-grid" style={{ gridTemplateColumns: 'minmax(0, 3fr) minmax(0, 1fr) auto', alignItems: 'end' }}>
                      <TextInput label={t('src.url')} value={p.url} onChange={(v) => setPlan(p.id, (x) => (x.url = v))} inputMode="url" spellCheck={false} />
                      <Select label={t('src.auth')} value={p.auth_token_ref ?? ''} options={[{ value: '', label: t('src.no_auth') }, ...creds.map((c) => ({ value: c.ref, label: c.ref }))]} onChange={(v) => setPlan(p.id, (x) => (x.auth_token_ref = v))} />
                      <Button variant="primary" loading={busy === `probe:${p.id}`} onClick={() => void probe(p)}>
                        <Icon name="fetch" size={16} /> {t('wz.probe')}
                      </Button>
                    </div>
                  ) : (
                    <div className="dim">{t('wz.static_source')}</div>
                  )}
                  {typeof pr === 'string' && <div className="notice danger">{pr}</div>}
                  {pr && typeof pr !== 'string' && pr.error && <div className="notice danger">{pr.error}</div>}
                  {pr && typeof pr !== 'string' && pr.format === 'json' && (
                    <details>
                      <summary className="muted">{t('wz.response', { n: pr.body_bytes })}</summary>
                      <div className="jt-box" style={{ marginTop: 8, maxHeight: 240 }}>
                        <JsonTree value={pr.json} onPick={() => undefined} />
                      </div>
                    </details>
                  )}
                </div>
              </div>
            );
          })}
          {plans.length > 0 && (
            <div>
              <Button size="sm" variant="ghost" onClick={addSource}>
                <Icon name="plus" size={15} /> {t('src.add')}
              </Button>
            </div>
          )}
        </div>
      )}

      {step === 2 && wb && (
        <div className="src-split">
          <div className="card">
            <div className="card-head">
              <h3 className="grow">{t('wz.map_title')}</h3>
              <Badge tone={mapped === panels.length ? 'ok' : 'warn'}>{t('wz.mapped', { n: mapped, total: panels.length })}</Badge>
              <Button size="sm" loading={busy === 'test'} onClick={() => void testAll()}>
                <Icon name="play" size={14} /> {t('src.run_preview')}
              </Button>
            </div>
            <div className="card-body col">
              {!panels.length && <div className="dim">{t('wz.no_data_panels')}</div>}
              {panels.map((p) => {
                const opts = p.source_id ? metricsForSource(wb, p.source_id, observed) : [];
                const val = (observed[p.source_id ?? ''] ?? []).find((m) => m.metric === p.metric)?.value;
                return (
                  <div key={p.id} className={`map-row${focusPanel === p.id ? ' on' : ''}`} onClick={() => setFocusPanel(p.id)}>
                    <div className="row nowrap" style={{ minWidth: 0 }}>
                      <Icon name={`k-${p.kind}`} />
                      <div className="grow" style={{ minWidth: 0 }}>
                        <div className="truncate" style={{ fontWeight: 600 }}>{p.title || p.id}</div>
                        <div className="dim">
                          {label('kind', p.kind)}
                          {val !== undefined && ` · ${val}`}
                        </div>
                      </div>
                    </div>
                    <div className="row nowrap">
                      <select className="input sm" aria-label={t('panel.source')} value={p.source_id ?? ''} onChange={(e) => setPanel(p.id, (x) => (x.source_id = e.target.value))} style={{ maxWidth: 120 }}>
                        <option value="">—</option>
                        {wb.dashboard.sources.map((s) => (
                          <option key={s.id} value={s.id}>
                            {s.id}
                          </option>
                        ))}
                      </select>
                      <select className="input sm" aria-label={t('panel.metric')} value={p.metric ?? ''} onChange={(e) => setPanel(p.id, (x) => (x.metric = e.target.value))}>
                        <option value="">{t('wz.pick_metric')}</option>
                        {p.metric && !opts.some((o) => o.name === p.metric) && <option value={p.metric}>{p.metric}</option>}
                        {opts.map((o) => (
                          <option key={o.name} value={o.name}>
                            {o.name}
                            {o.unit ? ` (${o.unit})` : ''}
                          </option>
                        ))}
                      </select>
                    </div>
                  </div>
                );
              })}
            </div>
          </div>
          <div className="card">
            <div className="card-head">
              <h3 className="grow">{t('wz.raw_title')}</h3>
            </div>
            <div className="card-body col">
              <div className="dim">{focusPanel ? t('wz.raw_hint_on', { name: panels.find((p) => p.id === focusPanel)?.title ?? focusPanel }) : t('wz.raw_hint')}</div>
              {Object.entries(probes).map(([id, pr]) =>
                typeof pr !== 'string' && pr.format === 'json' ? (
                  <div key={id}>
                    <div className="dim mono" style={{ marginBottom: 4 }}>{id}</div>
                    <div className="jt-box">
                      <JsonTree value={pr.json} onPick={(segs, v) => pickJson(id, segs, v)} />
                    </div>
                  </div>
                ) : null,
              )}
              {!Object.keys(probes).length && <div className="notice">{t('wz.raw_none')}</div>}
            </div>
          </div>
        </div>
      )}

      {step === 3 && wb && (
        <div className="src-split">
          <div className="card card-body col" style={{ gap: 14 }}>
            <h3>{t('wz.summary')}</h3>
            <div className="tiles">
              <div className="card tile">
                <span className="tile-label"><Icon name="layout" size={14} /> {t('editor.pages')}</span>
                <span className="tile-value">{wb.dashboard.pages.length}</span>
              </div>
              <div className="card tile">
                <span className="tile-label"><Icon name="k-card" size={14} /> {t('wz.panels')}</span>
                <span className="tile-value">{wb.dashboard.pages.reduce((n, p) => n + p.panels.length, 0)}</span>
              </div>
              <div className="card tile">
                <span className="tile-label"><Icon name="db" size={14} /> {t('nav.sources')}</span>
                <span className="tile-value">{wb.dashboard.sources.length}</span>
              </div>
              <div className="card tile">
                <span className="tile-label"><Icon name="rules" size={14} /> {t('nav.rules')}</span>
                <span className="tile-value">{wb.dashboard.alert_rules.length}</span>
              </div>
            </div>
            {mapped < panels.length && (
              <div className="notice warn">
                <Icon name="warn" size={16} />
                <span>{t('wz.unmapped', { n: panels.length - mapped })}</span>
              </div>
            )}
            <div className="notice">
              <Icon name="device" size={16} />
              <span>{t('wz.apply_note')}</span>
            </div>
          </div>
          <div className="card card-body" style={{ display: 'flex', justifyContent: 'center', background: 'var(--desk)' }}>
            {firstPage && <ScreenThumb page={firstPage} bundle={wb} observed={observed} width={280} />}
          </div>
        </div>
      )}

      <div className="wz-foot">
        <div className="row">
          {step > 0 ? (
            <Button onClick={() => setStep((s) => (s - 1) as Step)}>
              <Icon name="chevl" size={16} /> {t('wz.back')}
            </Button>
          ) : (
            <Button variant="ghost" onClick={skip}>
              {t('wz.skip')}
            </Button>
          )}
        </div>
        <div className="row">
          {step < 3 ? (
            <Button variant="primary" disabled={!wb} onClick={() => setStep((s) => (s + 1) as Step)}>
              {t('wz.next')} <Icon name="chevr" size={16} />
            </Button>
          ) : (
            <>
              <Button onClick={() => void finish(false)}>{t('wz.save_draft')}</Button>
              <Button variant="primary" loading={d.busy === 'apply'} onClick={() => void finish(true)}>
                <Icon name="send" size={16} /> {t('draft.apply')}
              </Button>
            </>
          )}
        </div>
      </div>
    </div>
  );
}
