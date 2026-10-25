// Side panel: per-kind panel forms (fields from dashboard_core model.h) and
// page / dashboard settings when nothing is selected.
import { useMemo, type ReactNode } from 'react';
import { useEnumLabel, useI18n } from '../../i18n';
import { useDraft } from '../../state/draft';
import { panelErrors, locateError } from '../../model/errors';
import { metricsForSource } from '../../model/metrics';
import { allPanelIds, uniqueId, findPanel } from '../../model/bundle';
import { canPlace, columnsOf, findFreeSpot, fitsScreen, MAX_COLUMNS, ROWS_PER_SCREEN } from '../../model/grid';
import { applyPlacement, defaultSize, resolvePlacement } from '../../model/layout';
import { ACTION_KINDS, CHART_KINDS, IMAGE_FITS, PANEL_KINDS, SEVERITIES, VARIANTS, type Action, type Bundle, type GridRect, type Panel, type PanelKind } from '../../model/types';
import { Button, Checkbox, Icon, NumberInput, Select, Switch, TextArea, TextInput, type Option } from '../../ui';
import { useToast } from '../../state/toast';
import { navigate } from '../../state/router';

/** Fill kind-specific sub-objects the firmware expects for a kind. */
export function normalizeForKind(p: Panel, kind: PanelKind): Panel {
  const next: Panel = { ...p, kind, style: { ...(p.style ?? {}), variant: VARIANTS[kind].includes(p.style?.variant ?? 'default') ? p.style?.variant ?? 'default' : 'default' } };
  delete next.text;
  delete next.chart;
  delete next.columns;
  delete next.image;
  if (kind === 'text' || kind === 'card' || kind === 'alert') next.text = p.text ?? { content: '', multiline: false, max_lines: 1 };
  if (kind === 'chart') next.chart = p.chart ?? { kind: 'line', show_points: false, fill: false, y_min: null, y_max: null };
  if (kind === 'table') next.columns = p.columns?.length ? p.columns : [{ key: 'value', title: 'Value', unit: '', width: 6 }];
  if (kind === 'image') next.image = p.image ?? { asset_id: '', alt_text: '', fit: 'contain' };
  if (kind === 'button' && !next.actions?.length) next.actions = [{ id: 'refresh', kind: 'refresh', label: 'Refresh', target: 'current', value: '', confirm: false }];
  return next;
}

export function newPanel(b: Bundle, pageId: string, kind: PanelKind, title: string, at?: GridRect): Panel | null {
  const page = b.dashboard.pages.find((p) => p.id === pageId);
  if (!page) return null;
  const cols = columnsOf(page);
  const size = defaultSize(kind, cols);
  const spot = at ?? findFreeSpot(page, size.w, size.h) ?? findFreeSpot(page, Math.max(1, Math.round(cols / 4)), 1);
  if (!spot) return null;
  return normalizeForKind({ id: uniqueId(kind, allPanelIds(b.dashboard)), kind, grid: spot, title, visible: true, refresh_ms: 60000, style: { variant: 'default', precision: 1 } }, kind);
}

function Section({ title, children }: { title: string; children: ReactNode }) {
  return (
    <div className="section">
      <h4>{title}</h4>
      {children}
    </div>
  );
}

export function PanelInspector({ panelId, onDelete, onDuplicate }: { panelId: string; onDelete: (id: string) => void; onDuplicate: (id: string) => void }) {
  const { t } = useI18n();
  const label = useEnumLabel();
  const toast = useToast();
  const d = useDraft();
  const found = findPanel(d.draft.dashboard, panelId);
  const errs = useMemo(() => panelErrors(d.draft, d.errors, panelId), [d.draft, d.errors, panelId]);
  if (!found) return null;
  const { page, panel: p } = found;
  const key = (f: string) => `panel:${panelId}:${f}`;
  const set = (fn: (p: Panel) => void, f = 'x') =>
    d.update((b) => {
      const hit = findPanel(b.dashboard, panelId);
      if (hit) fn(hit.panel);
    }, key(f));
  const err = (f: string) => errs[f];

  const sources: Option[] = [{ value: '', label: t('common.none') }, ...d.draft.dashboard.sources.map((s) => ({ value: s.id, label: s.id }))];
  const metrics = p.source_id ? metricsForSource(d.draft, p.source_id, d.observed) : [];
  const listId = `metrics-${panelId}`;
  const samePagePanels = page.panels.filter((x) => x.id !== p.id);
  const variant = p.style?.variant ?? 'default';
  const columns = columnsOf(page);

  const setGrid = (k: 'x' | 'y' | 'w' | 'h', v: number | undefined) => {
    if (v === undefined) return;
    const g = { ...p.grid, [k]: Math.max(k === 'w' || k === 'h' ? 1 : 0, Math.round(v)) };
    if (p.visible === false) return fitsScreen(g) && g.x + g.w <= columns ? set((x) => (x.grid = g), 'grid') : toast('warn', t('editor.blocked'));
    if (g.x + g.w > columns || !fitsScreen(g)) return toast('warn', t('editor.blocked'));
    const placement = resolvePlacement(page, p.id, g);
    if (!placement.ok) return toast('warn', t('editor.blocked'));
    d.update((b) => {
      const pg = b.dashboard.pages.find((x) => x.id === page.id);
      if (pg) applyPlacement(pg, placement);
    }, key('grid'));
  };

  const actionTargets = (a: Action): Option[] | null => {
    if (a.kind === 'goto_page') return d.draft.dashboard.pages.map((x) => ({ value: x.id, label: x.title || x.id }));
    if (a.kind === 'goto_panel') return d.draft.dashboard.pages.flatMap((pg) => pg.panels.map((x) => ({ value: x.id, label: `${pg.id} / ${x.id}` })));
    if (a.kind === 'set_variable') return d.draft.dashboard.variables.map((v) => ({ value: v.name, label: v.label || v.name }));
    if (a.kind === 'refresh') return [{ value: 'all', label: 'all' }, { value: 'current', label: 'current' }, ...d.draft.dashboard.sources.map((s) => ({ value: s.id, label: s.id }))];
    if (a.kind === 'ack_alert') return [{ value: 'current', label: 'current' }, { value: 'focused', label: 'focused' }];
    return null;
  };

  return (
    <div className="col">
      <div className="row nowrap">
        <span className="page-icon" style={{ width: 34, height: 34, borderRadius: 9 }}>
          <Icon name={`k-${p.kind}`} size={18} />
        </span>
        <div className="grow">
          <h3 className="truncate">{p.title || p.id}</h3>
          <div className="dim">
            {label('kind', p.kind)} · {t('editor.size', { w: p.grid.w, h: p.grid.h })}
          </div>
        </div>
        <Button size="sm" icon variant="ghost" title={`${t('panel.duplicate')} (Ctrl+D)`} onClick={() => onDuplicate(p.id)}>
          <Icon name="copy" size={16} />
        </Button>
        <Button size="sm" icon variant="ghost" title={`${t('panel.delete')} (Delete)`} onClick={() => onDelete(p.id)}>
          <Icon name="trash" size={16} />
        </Button>
        <Button size="sm" icon variant="ghost" title={t('common.close')} onClick={() => d.setPanelId(null)}>
          <Icon name="x" size={16} />
        </Button>
      </div>
      {errs._ && <div className="notice danger">{errs._}</div>}
      <div className="form-grid two">
        <Select label={t('panel.kind')} value={p.kind} options={PANEL_KINDS.map((k) => ({ value: k, label: label('kind', k) }))} onChange={(v) => d.update((b) => {
          const hit = findPanel(b.dashboard, panelId);
          if (!hit) return;
          const i = hit.page.panels.indexOf(hit.panel);
          hit.page.panels[i] = normalizeForKind(hit.panel, v as PanelKind);
        })} error={err('kind')} />
        <Select label={t('panel.variant')} value={variant} options={VARIANTS[p.kind].map((v) => ({ value: v, label: label('variant', v) }))} onChange={(v) => set((x) => (x.style = { ...x.style, variant: v }), 'variant')} error={err('style.variant') || err('style')} />
        <TextInput wide label={t('panel.title')} value={p.title} onChange={(v) => set((x) => (x.title = v), 'title')} error={err('title')} />
        <TextInput
          label={t('panel.id')}
          value={p.id}
          error={err('id')}
          pattern="[A-Za-z0-9_-]+"
          onChange={(v) => {
            const id = v.replace(/[^A-Za-z0-9_-]/g, '');
            if (!id || allPanelIds(d.draft.dashboard).includes(id)) return;
            d.update((b) => {
              for (const pg of b.dashboard.pages)
                for (const x of pg.panels) {
                  if (x.id === p.id) x.id = id;
                  if (x.linked_panel_ids) x.linked_panel_ids = x.linked_panel_ids.map((l) => (l === p.id ? id : l));
                  for (const a of x.actions ?? []) if (a.kind === 'goto_panel' && a.target === p.id) a.target = id;
                }
            }, key('id'));
            d.setPanelId(id);
          }}
        />
        <Switch label={t('panel.visible')} checked={p.visible !== false} onChange={(v) => {
          if (v && !canPlace(page, p.grid, p.id)) {
            const spot = findFreeSpot(page, p.grid.w, Math.min(p.grid.h, ROWS_PER_SCREEN), 0, p.id);
            if (!spot) return toast('warn', t('editor.no_space'));
            set((x) => { x.visible = true; x.grid = spot; }, 'visible');
          } else set((x) => (x.visible = v), 'visible');
        }} />
      </div>

      <Section title={t('panel.position')}>
        <div className="form-grid" style={{ gridTemplateColumns: 'repeat(4, 1fr)' }}>
          <NumberInput label="x" min={0} max={columns - 1} value={p.grid.x} onChange={(v) => setGrid('x', v)} />
          <NumberInput label="y" min={0} max={ROWS_PER_SCREEN - 1} value={p.grid.y} onChange={(v) => setGrid('y', v)} />
          <NumberInput label="w" min={1} max={columns} value={p.grid.w} onChange={(v) => setGrid('w', v)} />
          <NumberInput label="h" min={1} max={ROWS_PER_SCREEN} value={p.grid.h} onChange={(v) => setGrid('h', v)} />
        </div>
        {err('grid') && <div className="err-text">{err('grid')}</div>}
      </Section>

      {p.kind !== 'button' && p.kind !== 'image' && (
        <Section title={t('panel.source')}>
          <div className="form-grid two">
            <Select label={t('panel.source')} value={p.source_id ?? ''} options={sources} onChange={(v) => set((x) => (x.source_id = v || undefined), 'source')} error={err('source_id')} />
            <TextInput label={t('panel.metric')} hint={metrics.length ? t('panel.metric_hint') : undefined} list={listId} value={p.metric} onChange={(v) => set((x) => (x.metric = v), 'metric')} error={err('metric')} />
            <datalist id={listId}>
              {metrics.map((m) => (
                <option key={m.name} value={m.name}>
                  {m.unit ? `${m.name} (${m.unit})` : m.name}
                </option>
              ))}
            </datalist>
            {p.source_id && (
              <div className="wide">
                <Button size="sm" variant="ghost" onClick={() => navigate('sources', p.source_id)}>
                  {t('nav.sources')} → {p.source_id}
                </Button>
              </div>
            )}
            <TextInput label={t('panel.value_path')} value={p.value_path} onChange={(v) => set((x) => (x.value_path = v || undefined), 'value_path')} error={err('value_path')} />
            <TextInput label={t('panel.value_expression')} value={p.value_expression} onChange={(v) => set((x) => (x.value_expression = v || undefined), 'expr')} error={err('value_expression')} />
            <NumberInput label={t('panel.refresh_ms')} min={0} step={1000} value={p.refresh_ms ?? 60000} onChange={(v) => v !== undefined && set((x) => (x.refresh_ms = v), 'refresh')} error={err('refresh_ms')} />
            <div />
            <TextInput label={t('panel.time_from')} value={p.time_window?.from ?? '-1h'} onChange={(v) => set((x) => (x.time_window = { from: v, to: x.time_window?.to ?? 'now' }), 'tw')} error={err('time_window')} />
            <TextInput label={t('panel.time_to')} value={p.time_window?.to ?? 'now'} onChange={(v) => set((x) => (x.time_window = { from: x.time_window?.from ?? '-1h', to: v }), 'tw2')} />
          </div>
        </Section>
      )}

      {(p.kind === 'card' || p.kind === 'chart' || p.kind === 'table') && (
        <Section title={t('panel.unit')}>
          <div className="form-grid two">
            <TextInput label={t('panel.unit')} value={p.style?.unit} onChange={(v) => set((x) => (x.style = { ...x.style, unit: v }), 'unit')} error={err('style.unit')} />
            <NumberInput label={t('panel.precision')} min={0} max={6} value={p.style?.precision ?? 2} onChange={(v) => v !== undefined && set((x) => (x.style = { ...x.style, precision: v }), 'precision')} />
            {p.kind === 'chart' && <NumberInput label={t('panel.line_width')} min={1} max={8} value={p.style?.line_width ?? 1} onChange={(v) => v !== undefined && set((x) => (x.style = { ...x.style, line_width: v }), 'lw')} />}
            {p.kind === 'chart' && <Checkbox label={t('panel.show_legend')} checked={p.style?.show_legend !== false} onChange={(v) => set((x) => (x.style = { ...x.style, show_legend: v }), 'legend')} />}
            <Checkbox label={t('panel.invert')} checked={!!p.style?.invert} onChange={(v) => set((x) => (x.style = { ...x.style, invert: v }), 'invert')} />
          </div>
        </Section>
      )}

      {(p.kind === 'card' || p.kind === 'chart' || p.kind === 'table' || p.kind === 'alert') && (
        <Section title={t('panel.thresholds')}>
          <div className="col">
            {(p.thresholds ?? []).map((th, i) => (
              <div key={i} className="row" style={{ alignItems: 'flex-end' }}>
                <div style={{ width: 90 }}>
                  <NumberInput label={t('panel.threshold_value')} value={th.value} onChange={(v) => v !== undefined && set((x) => (x.thresholds![i].value = v), `th${i}`)} />
                </div>
                <div style={{ width: 100 }}>
                  <Select label={t('panel.severity')} value={th.severity} options={SEVERITIES.map((s) => ({ value: s, label: label('severity', s) }))} onChange={(v) => set((x) => (x.thresholds![i].severity = v as typeof th.severity))} />
                </div>
                <div className="grow">
                  <TextInput label={t('panel.label')} value={th.label} onChange={(v) => set((x) => (x.thresholds![i].label = v), `thl${i}`)} />
                </div>
                <Button icon onClick={() => set((x) => x.thresholds!.splice(i, 1))} aria-label={t('common.delete')}>
                  <Icon name="x" />
                </Button>
              </div>
            ))}
            {err('thresholds') && <div className="err-text">{err('thresholds')}</div>}
            <div>
              <Button size="sm" onClick={() => set((x) => (x.thresholds = [...(x.thresholds ?? []), { value: 0, severity: 'warning', label: '' }]))}>
                <Icon name="plus" /> {t('panel.add_threshold')}
              </Button>
            </div>
          </div>
        </Section>
      )}

      {p.kind === 'card' && (
        <Section title={t('panel.linked')}>
          <div className="dim">{t('panel.linked_hint')}</div>
          <div className="col" style={{ marginTop: 6 }}>
            {samePagePanels.filter((x) => x.kind === 'card').map((x) => (
              <Checkbox
                key={x.id}
                label={`${x.title || x.id} (${x.id})${x.visible === false ? ' · hidden' : ''}`}
                checked={(p.linked_panel_ids ?? []).includes(x.id)}
                onChange={(on) => set((y) => (y.linked_panel_ids = on ? [...(y.linked_panel_ids ?? []), x.id] : (y.linked_panel_ids ?? []).filter((id) => id !== x.id)))}
              />
            ))}
            {(variant === 'dual' || variant === 'triple') && (
              <Button
                size="sm"
                onClick={() =>
                  d.update((b) => {
                    const hit = findPanel(b.dashboard, panelId)!;
                    const id = uniqueId(`${p.id}_linked`, allPanelIds(b.dashboard));
                    hit.page.panels.push({ id, kind: 'card', grid: { x: 0, y: 0, w: 1, h: 1 }, title: 'Linked', visible: false, source_id: p.source_id, metric: '', style: { variant: 'default', precision: p.style?.precision ?? 1 } });
                    hit.panel.linked_panel_ids = [...(hit.panel.linked_panel_ids ?? []), id];
                  })
                }
              >
                <Icon name="plus" /> {t('editor.hidden_panels')}
              </Button>
            )}
            {err('linked_panel_ids') && <div className="err-text">{err('linked_panel_ids')}</div>}
          </div>
        </Section>
      )}

      {p.text && (
        <Section title={t('panel.text')}>
          <div className="form-grid two">
            <TextArea label={t('panel.text')} hint={t('panel.text_hint')} value={p.text.content} onChange={(v) => set((x) => (x.text = { ...x.text!, content: v }), 'text')} error={err('text.content') || err('text')} rows={3} />
            <Checkbox label={t('panel.multiline')} checked={!!p.text.multiline} onChange={(v) => set((x) => (x.text = { ...x.text!, multiline: v, max_lines: v ? Math.max(2, x.text!.max_lines ?? 1) : 1 }))} />
            <NumberInput label={t('panel.max_lines')} min={1} max={64} value={p.text.max_lines ?? 1} onChange={(v) => v !== undefined && set((x) => (x.text = { ...x.text!, max_lines: v }), 'ml')} error={err('text.max_lines')} />
          </div>
        </Section>
      )}

      {p.chart && (
        <Section title={t('panel.chart')}>
          <div className="form-grid two">
            <Select label={t('panel.chart_kind')} value={p.chart.kind} options={CHART_KINDS.map((k) => ({ value: k, label: label('chart', k) }))} onChange={(v) => set((x) => (x.chart = { ...x.chart!, kind: v as typeof p.chart.kind }))} />
            <div />
            <Checkbox label={t('panel.show_points')} checked={!!p.chart.show_points} onChange={(v) => set((x) => (x.chart = { ...x.chart!, show_points: v }))} />
            <Checkbox label={t('panel.fill')} checked={!!p.chart.fill} onChange={(v) => set((x) => (x.chart = { ...x.chart!, fill: v }))} />
            <NumberInput allowEmpty placeholder={t('panel.auto')} label={t('panel.y_min')} value={p.chart.y_min} onChange={(v) => set((x) => (x.chart = { ...x.chart!, y_min: v ?? null }), 'ymin')} error={err('chart.y_min')} />
            <NumberInput allowEmpty placeholder={t('panel.auto')} label={t('panel.y_max')} value={p.chart.y_max} onChange={(v) => set((x) => (x.chart = { ...x.chart!, y_max: v ?? null }), 'ymax')} error={err('chart.y_max') || err('chart')} />
          </div>
        </Section>
      )}

      {p.columns && (
        <Section title={t('panel.columns')}>
          <div className="col">
            {p.columns.map((c, i) => (
              <div key={i} className="subcard form-grid two">
                <TextInput label={t('panel.column_key')} value={c.key} onChange={(v) => set((x) => (x.columns![i].key = v), `ck${i}`)} error={err(`columns[${i}].key`)} />
                <TextInput label={t('panel.title')} value={c.title} onChange={(v) => set((x) => (x.columns![i].title = v), `ct${i}`)} />
                <TextInput label={t('panel.unit')} value={c.unit} onChange={(v) => set((x) => (x.columns![i].unit = v), `cu${i}`)} />
                <div className="row" style={{ alignItems: 'flex-end' }}>
                  <div className="grow">
                    <NumberInput label={t('panel.column_width')} min={1} value={c.width ?? 4} onChange={(v) => v !== undefined && set((x) => (x.columns![i].width = v), `cw${i}`)} />
                  </div>
                  <Button icon onClick={() => set((x) => x.columns!.splice(i, 1))} aria-label={t('common.delete')}>
                    <Icon name="x" />
                  </Button>
                </div>
              </div>
            ))}
            <div>
              <Button size="sm" onClick={() => set((x) => x.columns!.push({ key: `col${x.columns!.length + 1}`, title: '', unit: '', width: 4 }))}>
                <Icon name="plus" /> {t('panel.add_column')}
              </Button>
            </div>
          </div>
        </Section>
      )}

      {p.image && (
        <Section title={t('panel.image')}>
          <div className="form-grid two">
            <Select
              label={t('panel.asset')}
              value={p.image.asset_id}
              options={[{ value: '', label: t('common.none') }, ...d.draft.dashboard.assets.filter((a) => a.kind === 'image').map((a) => ({ value: a.id, label: a.id }))]}
              onChange={(v) => set((x) => (x.image = { ...x.image!, asset_id: v }))}
              error={err('image.asset_id') || err('image')}
            />
            <Select label={t('panel.fit')} value={p.image.fit ?? 'contain'} options={IMAGE_FITS.map((f) => ({ value: f, label: label('fit', f) }))} onChange={(v) => set((x) => (x.image = { ...x.image!, fit: v as typeof p.image.fit }))} />
            <TextInput wide label={t('panel.alt')} value={p.image.alt_text} onChange={(v) => set((x) => (x.image = { ...x.image!, alt_text: v }), 'alt')} />
          </div>
        </Section>
      )}

      <Section title={t('panel.actions')}>
        <div className="col">
          {(p.actions ?? []).map((a, i) => {
            const targets = actionTargets(a);
            return (
              <div key={i} className="subcard form-grid two">
                <Select label={t('panel.action_kind')} value={a.kind} options={ACTION_KINDS.map((k) => ({ value: k, label: label('action', k) }))} onChange={(v) => set((x) => (x.actions![i] = { ...x.actions![i], kind: v as Action['kind'], target: v === 'back' ? '' : v === 'refresh' || v === 'ack_alert' ? 'current' : '' }))} error={err(`actions[${i}].kind`)} />
                <TextInput label={t('panel.label')} value={a.label} onChange={(v) => set((x) => (x.actions![i].label = v), `al${i}`)} />
                {targets && <Select label={t('panel.action_target')} value={a.target ?? ''} options={targets} onChange={(v) => set((x) => (x.actions![i].target = v))} error={err(`actions[${i}].target`)} />}
                {a.kind === 'set_variable' && <TextInput label={t('panel.action_value')} value={a.value} onChange={(v) => set((x) => (x.actions![i].value = v), `av${i}`)} />}
                <Checkbox label={t('panel.action_confirm')} checked={!!a.confirm} onChange={(v) => set((x) => (x.actions![i].confirm = v))} />
                <div className="row" style={{ justifyContent: 'flex-end' }}>
                  <Button size="sm" variant="danger" onClick={() => set((x) => x.actions!.splice(i, 1))}>
                    {t('common.delete')}
                  </Button>
                </div>
              </div>
            );
          })}
          <div>
            <Button size="sm" onClick={() => set((x) => (x.actions = [...(x.actions ?? []), { id: uniqueId('action', (x.actions ?? []).map((y) => y.id)), kind: 'refresh', label: 'Refresh', target: 'current', value: '', confirm: false }]))}>
              <Icon name="plus" /> {t('panel.add_action')}
            </Button>
          </div>
        </div>
      </Section>
    </div>
  );
}

export function PageInspector() {
  const { t } = useI18n();
  const d = useDraft();
  const page = d.draft.dashboard.pages.find((p) => p.id === d.pageId);
  const dash = d.draft.dashboard;
  const dashErrors = d.errors.map((e) => ({ e, at: locateError(d.draft, e.field) }));
  const pageErr = (f: string) => dashErrors.find((x) => x.at.area === 'page' && x.at.pageId === page?.id && x.at.field === f)?.e.message;
  const dashErr = (f: string) => dashErrors.find((x) => x.at.area === 'dashboard' && x.at.field === f)?.e.message;
  const hidden = page?.panels.filter((p) => p.visible === false) ?? [];
  const pageIndex = page ? dash.pages.indexOf(page) : -1;
  const movePage = (dir: -1 | 1) =>
    d.update((b) => {
      const ps = b.dashboard.pages;
      const j = pageIndex + dir;
      if (pageIndex < 0 || j < 0 || j >= ps.length) return;
      [ps[pageIndex], ps[j]] = [ps[j], ps[pageIndex]];
    });
  const deletePage = () => {
    if (!page || dash.pages.length <= 1) return;
    if (!confirm(t('editor.delete_page_confirm', { name: page.title || page.id }))) return;
    d.update((b) => {
      b.dashboard.pages = b.dashboard.pages.filter((p) => p.id !== page.id);
      if (b.dashboard.default_page_id === page.id) b.dashboard.default_page_id = b.dashboard.pages[0].id;
    });
  };
  return (
    <div className="col">
      <div className="dim">{t('editor.no_selection')}</div>
      {page && (
        <div className="section">
          <div className="row" style={{ marginBottom: 8 }}>
            <h4 className="grow">{t('editor.page_settings')}</h4>
            <Button size="sm" icon title={t('editor.move_up')} disabled={pageIndex === 0} onClick={() => movePage(-1)}>
              <Icon name="up" />
            </Button>
            <Button size="sm" icon title={t('editor.move_down')} disabled={pageIndex === dash.pages.length - 1} onClick={() => movePage(1)}>
              <Icon name="down" />
            </Button>
            <Button size="sm" icon variant="danger" title={t('editor.delete_page')} disabled={dash.pages.length <= 1} onClick={deletePage}>
              <Icon name="trash" />
            </Button>
          </div>
          <div className="form-grid two">
            <TextInput wide label={t('editor.page_title')} value={page.title} onChange={(v) => d.update((b) => (b.dashboard.pages.find((p) => p.id === page.id)!.title = v), `page:${page.id}:title`)} error={pageErr('title')} />
            <TextInput
              label={t('editor.page_id')}
              value={page.id}
              error={pageErr('id')}
              onChange={(v) => {
                const id = v.replace(/[^A-Za-z0-9_-]/g, '');
                if (!id || dash.pages.some((p) => p.id === id)) return;
                d.update((b) => {
                  const pg = b.dashboard.pages.find((p) => p.id === page.id)!;
                  pg.id = id;
                  if (b.dashboard.default_page_id === page.id) b.dashboard.default_page_id = id;
                  for (const x of b.dashboard.pages) for (const pn of x.panels) for (const a of pn.actions ?? []) if (a.kind === 'goto_page' && a.target === page.id) a.target = id;
                }, `page:${page.id}:id`);
                d.setPageId(id);
              }}
            />
            <NumberInput
              label={t('editor.columns')}
              min={1}
              max={MAX_COLUMNS}
              value={page.grid_columns ?? 24}
              error={pageErr('grid_columns')}
              onChange={(v) => {
                if (v === undefined || v < 1 || v > MAX_COLUMNS) return;
                const old = columnsOf(page);
                d.update((b) => {
                  const pg = b.dashboard.pages.find((p) => p.id === page.id)!;
                  pg.grid_columns = Math.round(v);
                  // Rescale panels proportionally so the layout keeps its shape.
                  for (const pn of pg.panels) {
                    const x = Math.round((pn.grid.x * v) / old);
                    const x2 = Math.round(((pn.grid.x + pn.grid.w) * v) / old);
                    pn.grid = { ...pn.grid, x: Math.min(x, v - 1), w: Math.max(1, Math.min(v, x2) - Math.min(x, v - 1)) };
                  }
                }, `page:${page.id}:cols`);
              }}
            />
            <Checkbox label={t('editor.visible')} checked={page.visible !== false} onChange={(v) => d.update((b) => (b.dashboard.pages.find((p) => p.id === page.id)!.visible = v))} />
            <Checkbox label={t('editor.default_page')} checked={dash.default_page_id === page.id} onChange={(v) => v && d.update((b) => (b.dashboard.default_page_id = page.id))} />
          </div>
          {hidden.length > 0 && (
            <div className="section">
              <h4>{t('editor.hidden_panels')}</h4>
              <div className="col">
                {hidden.map((p) => (
                  <Button key={p.id} size="sm" variant="ghost" onClick={() => d.setPanelId(p.id)}>
                    {p.title || p.id} <span className="dim">({p.id})</span>
                  </Button>
                ))}
              </div>
            </div>
          )}
        </div>
      )}
      <div className="section">
        <h4>{t('editor.dashboard_settings')}</h4>
        <div className="form-grid two">
          <TextInput label={t('editor.dashboard_id')} value={dash.id} onChange={(v) => d.update((b) => (b.dashboard.id = v.replace(/[^A-Za-z0-9_-]/g, '')), 'dash:id')} error={dashErr('id')} />
          <TextInput label={t('editor.dashboard_title')} value={dash.title} onChange={(v) => d.update((b) => (b.dashboard.title = v), 'dash:title')} error={dashErr('title')} />
          <NumberInput label={t('editor.refresh_ms')} min={1000} step={1000} value={dash.refresh_ms ?? 60000} onChange={(v) => v !== undefined && d.update((b) => (b.dashboard.refresh_ms = v), 'dash:refresh')} error={dashErr('refresh_ms')} />
        </div>
      </div>
      <div className="section">
        <h4>{t('editor.variables')}</h4>
        <div className="col">
          {dash.variables.map((v, i) => (
            <div key={i} className="subcard form-grid two">
              <TextInput label="name" value={v.name} onChange={(x) => d.update((b) => (b.dashboard.variables[i].name = x.replace(/[^A-Za-z0-9_]/g, '')), `var${i}n`)} />
              <TextInput label={t('panel.label')} value={v.label} onChange={(x) => d.update((b) => (b.dashboard.variables[i].label = x), `var${i}l`)} />
              <TextInput label="default" value={v.default_value} onChange={(x) => d.update((b) => (b.dashboard.variables[i].default_value = x), `var${i}d`)} />
              <div className="row" style={{ alignItems: 'flex-end', justifyContent: 'flex-end' }}>
                <Button size="sm" variant="danger" onClick={() => d.update((b) => b.dashboard.variables.splice(i, 1))}>
                  {t('common.delete')}
                </Button>
              </div>
            </div>
          ))}
          <div>
            <Button size="sm" onClick={() => d.update((b) => b.dashboard.variables.push({ name: uniqueId('var', b.dashboard.variables.map((x) => x.name)).replace(/-/g, '_'), label: '', kind: 'text', default_value: '', options: [], source_id: '', value_path: '', multi: false, include_all: false }))}>
              <Icon name="plus" /> {t('common.add')}
            </Button>
          </div>
        </div>
      </div>
    </div>
  );
}
